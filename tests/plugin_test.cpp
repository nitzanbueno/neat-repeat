// Offline tests that drive the plugin through its CLAP entry point.
//
// They run at 1000 Hz so one sample is one millisecond, and feed the sample
// index as the input signal: an output value therefore says exactly which
// input moment is being played back.

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "length.h"


namespace {

using namespace neat;

constexpr clap_id kTrigger = 0, kLength = 1, kSync = 2;
constexpr double kRate = 1000.0;
constexpr uint32_t kBlock = 64;

int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

const clap_host kHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "neat-repeat-tests",
    "",
    "",
    "1",
    [](const clap_host*, const char*) -> const void* { return nullptr; },
    [](const clap_host*) {},
    [](const clap_host*) {},
    [](const clap_host*) {},
};

struct EventList {
  std::vector<clap_event_param_value> events;
  clap_input_events list{this,
                         [](const clap_input_events* l) -> uint32_t {
                           return static_cast<uint32_t>(
                               static_cast<const EventList*>(l->ctx)->events.size());
                         },
                         [](const clap_input_events* l, uint32_t i) -> const clap_event_header* {
                           return &static_cast<const EventList*>(l->ctx)->events[i].header;
                         }};
};

const clap_output_events kNoOutput = {
    nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};

clap_event_param_value paramEvent(uint32_t time, clap_id id, double value) {
  clap_event_param_value ev{};
  ev.header.size = sizeof(ev);
  ev.header.time = time;
  ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  ev.header.type = CLAP_EVENT_PARAM_VALUE;
  ev.param_id = id;
  ev.note_id = -1;
  ev.port_index = -1;
  ev.channel = -1;
  ev.key = -1;
  ev.value = value;
  return ev;
}

struct Harness {
  const clap_plugin* plugin = nullptr;
  const clap_plugin_params* params = nullptr;
  const clap_plugin_state* state = nullptr;
  double tempo = 120.0;
  int64_t now = 0;
  std::vector<float> outL, outR;

  struct Change {
    int64_t time;
    clap_id id;
    double value;
  };
  std::vector<Change> changes;

  Harness() {
    clap_entry.init("");
    auto* factory = static_cast<const clap_plugin_factory*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    plugin = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
    plugin->init(plugin);
    params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    plugin->activate(plugin, kRate, 1, kBlock);
    plugin->start_processing(plugin);
  }

  ~Harness() {
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    clap_entry.deinit();
  }

  // Schedules a parameter change at an absolute sample time.
  void set(int64_t time, clap_id id, double value) { changes.push_back({time, id, value}); }

  // Processes audio up to (not including) the absolute sample time `until`,
  // in place, with the input signal L = t, R = -t.
  void run(int64_t until) {
    while (now < until) {
      const uint32_t frames = static_cast<uint32_t>(std::min<int64_t>(kBlock, until - now));
      std::vector<float> left(frames), right(frames);
      for (uint32_t i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(now + i);
        right[i] = -static_cast<float>(now + i);
      }
      float* channels[2] = {left.data(), right.data()};
      clap_audio_buffer io{};
      io.data32 = channels;
      io.channel_count = 2;

      EventList events;
      for (const Change& c : changes) {
        if (c.time >= now && c.time < now + frames)
          events.events.push_back(paramEvent(static_cast<uint32_t>(c.time - now), c.id, c.value));
      }
      std::stable_sort(events.events.begin(), events.events.end(),
                       [](const auto& a, const auto& b) { return a.header.time < b.header.time; });

      clap_event_transport transport{};
      transport.header.size = sizeof(transport);
      transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      transport.header.type = CLAP_EVENT_TRANSPORT;
      transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
      transport.tempo = tempo;

      clap_process proc{};
      proc.steady_time = now;
      proc.frames_count = frames;
      proc.transport = &transport;
      proc.audio_inputs = &io;
      proc.audio_outputs = &io;
      proc.audio_inputs_count = 1;
      proc.audio_outputs_count = 1;
      proc.in_events = &events.list;
      proc.out_events = &kNoOutput;
      plugin->process(plugin, &proc);

      outL.insert(outL.end(), left.begin(), left.end());
      outR.insert(outR.end(), right.begin(), right.end());
      now += frames;
    }
  }

  // Checks output over [from, to) against expected(t). Reports the first miss.
  void expect(int64_t from, int64_t to, const std::function<int64_t(int64_t)>& expected) {
    for (int64_t t = from; t < to; ++t) {
      const float want = static_cast<float>(expected(t));
      if (outL[t] != want || outR[t] != -want) {
        std::printf("  FAILED at t=%lld: got %g / %g, expected %g\n", static_cast<long long>(t),
                    outL[t], outR[t], want);
        ++failures;
        return;
      }
    }
  }

  std::string text(clap_id id, double value) {
    char buf[CLAP_NAME_SIZE];
    params->value_to_text(plugin, id, value, buf, sizeof(buf));
    return buf;
  }

  // Applies parameter values outside of process(), the way a host would
  // before playback starts.
  void flush(clap_id id, double value) {
    EventList events;
    events.events.push_back(paramEvent(0, id, value));
    params->flush(plugin, &events.list, &kNoOutput);
  }
};

void testPassThrough() {
  Harness h;
  h.run(500);
  h.expect(0, 500, [](int64_t t) { return t; });
}

void testBasicLoop() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(1000, kTrigger, 1);
  h.run(1400);
  h.expect(0, 1000, [](int64_t t) { return t; });
  h.expect(1000, 1400, [](int64_t t) { return 1000 + (t - 1000) % 100; });
}

// 100 ms loop, 50 ms in, length -> 150 ms: plays another 100 ms, then loops.
void testLengthIncrease() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(1000, kTrigger, 1);
  h.set(1150, kLength, normFromMs(150));  // second pass, 50 ms in
  h.run(1700);
  h.expect(1000, 1150, [](int64_t t) { return 1000 + (t - 1000) % 100; });
  h.expect(1150, 1250, [](int64_t t) { return t - 100; });  // positions 50..149
  h.expect(1250, 1700, [](int64_t t) { return 1000 + (t - 1250) % 150; });
}

// Shorter, but the playhead hasn't reached the new end yet: loops at the new end.
void testLengthDecreaseBeforeEnd() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(1000, kTrigger, 1);
  h.set(1030, kLength, normFromMs(50));
  h.run(1300);
  h.expect(1000, 1050, [](int64_t t) { return t; });
  h.expect(1050, 1300, [](int64_t t) { return 1000 + (t - 1050) % 50; });
}

// 100 ms loop, 75 ms in, length -> 50 ms: restarts right away as a new trigger.
void testLengthDecreasePastEnd() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(1000, kTrigger, 1);
  h.set(1075, kLength, normFromMs(50));
  h.run(1400);
  h.expect(1000, 1075, [](int64_t t) { return t; });
  h.expect(1075, 1400, [](int64_t t) { return 1075 + (t - 1075) % 50; });
}

void testRetriggerAndOff() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(1000, kTrigger, 1);
  h.set(1250, kTrigger, 2);
  h.set(1420, kTrigger, 1);
  h.set(1500, kTrigger, 0);
  h.run(1700);
  h.expect(1000, 1250, [](int64_t t) { return 1000 + (t - 1000) % 100; });
  h.expect(1250, 1420, [](int64_t t) { return 1250 + (t - 1250) % 100; });
  h.expect(1420, 1500, [](int64_t t) { return 1420 + (t - 1420) % 100; });
  h.expect(1500, 1700, [](int64_t t) { return t; });
}

int syncIndexOf(const char* name) {
  for (int i = 0; i < kNumSyncDivisions; ++i)
    if (std::strcmp(kSyncDivisions[i].name, name) == 0) return i;
  return -1;
}

void testSyncLength() {
  Harness h;  // 120 BPM: a quarter note is 500 samples
  h.set(0, kSync, 1);
  h.set(0, kLength, normFromSyncIndex(syncIndexOf("1/4")));
  h.set(1000, kTrigger, 1);
  h.run(2600);
  h.expect(1000, 2600, [](int64_t t) { return 1000 + (t - 1000) % 500; });
}

void testSyncTriplet() {
  Harness h;
  h.tempo = 100.0;  // 1/8T = 1/12 whole = 4 * 60 / 100 / 12 s = 200 ms
  h.set(0, kSync, 1);
  h.set(0, kLength, normFromSyncIndex(syncIndexOf("1/8T")));
  h.set(1000, kTrigger, 1);
  h.run(1700);
  h.expect(1000, 1700, [](int64_t t) { return 1000 + (t - 1000) % 200; });
}

// Toggling Sync while looping is a length change like any other.
void testSyncToggleWhileLooping() {
  Harness h;
  h.set(0, kLength, normFromSyncIndex(syncIndexOf("1/16")));  // 100 ms / 125 ms at 120 BPM
  h.set(1000, kTrigger, 1);
  h.set(1050, kSync, 1);
  h.run(1500);
  h.expect(1000, 1125, [](int64_t t) { return t; });
  h.expect(1125, 1500, [](int64_t t) { return 1000 + (t - 1125) % 125; });
}

void testMaxLength() {
  Harness h;
  h.set(0, kLength, 1.0);  // 10 s
  h.set(100, kTrigger, 2);
  h.run(100 + 25000);
  h.expect(100, 100 + 25000, [](int64_t t) { return 100 + (t - 100) % 10000; });
}

// After 10 s of capture, the length can still grow to the 10 s maximum.
void testGrowAfterLongLoop() {
  Harness h;
  h.set(0, kLength, normFromMs(100));
  h.set(0, kTrigger, 1);
  h.set(12030, kLength, 1.0);  // well past 10 s of capture, 30 ms into the loop
  h.run(30000);
  h.expect(0, 12030, [](int64_t t) { return t % 100; });
  h.expect(12030, 22000, [](int64_t t) { return t - 12000; });
  h.expect(22000, 30000, [](int64_t t) { return (t - 22000) % 10000; });
}

void testParamText() {
  Harness h;
  CHECK(h.text(kTrigger, 0) == "Off");
  CHECK(h.text(kTrigger, 1) == "On 1");
  CHECK(h.text(kTrigger, 2) == "On 2");
  CHECK(h.text(kSync, 1) == "On");
  CHECK(h.text(kLength, normFromMs(10)) == "10.0 ms");
  CHECK(h.text(kLength, normFromMs(100)) == "100 ms");
  CHECK(h.text(kLength, 1.0) == "10.00 s");
  h.flush(kSync, 1);
  CHECK(h.text(kLength, 0.0) == "1/64");
  CHECK(h.text(kLength, 1.0) == "1 Bar");
  CHECK(h.text(kLength, normFromSyncIndex(1)) == "1/32T");
  CHECK(h.text(kLength, normFromSyncIndex(2)) == "1/64D");
  CHECK(h.text(kLength, normFromSyncIndex(3)) == "1/32");

  double v = -1;
  CHECK(h.params->text_to_value(h.plugin, kLength, "1/8D", &v) &&
        v == normFromSyncIndex(syncIndexOf("1/8D")));
  CHECK(h.params->text_to_value(h.plugin, kLength, "2.5 s", &v) &&
        std::fabs(v - normFromMs(2500)) < 1e-12);
  CHECK(h.params->text_to_value(h.plugin, kLength, "250", &v) &&
        std::fabs(v - normFromMs(250)) < 1e-12);
  CHECK(h.params->text_to_value(h.plugin, kTrigger, "on 2", &v) && v == 2);
  CHECK(!h.params->text_to_value(h.plugin, kLength, "banana", &v));

  clap_param_info info;
  CHECK(h.params->get_info(h.plugin, kTrigger, &info) && (info.flags & CLAP_PARAM_IS_STEPPED) &&
        info.max_value == 2);
  CHECK(h.params->get_info(h.plugin, kLength, &info) && !(info.flags & CLAP_PARAM_IS_STEPPED));
}

void testSyncDivisionOrder() {
  CHECK(kNumSyncDivisions == 19);
  for (int i = 1; i < kNumSyncDivisions; ++i)
    CHECK(kSyncDivisions[i - 1].wholeNotes < kSyncDivisions[i].wholeNotes);
}

void testStateRoundTrip() {
  std::vector<uint8_t> blob;
  {
    Harness h;
    h.flush(kTrigger, 2);
    h.flush(kLength, 0.75);
    h.flush(kSync, 1);
    clap_ostream os{&blob, [](const clap_ostream* s, const void* data, uint64_t size) -> int64_t {
                      auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
                      auto* p = static_cast<const uint8_t*>(data);
                      b->insert(b->end(), p, p + size);
                      return static_cast<int64_t>(size);
                    }};
    CHECK(h.state->save(h.plugin, &os));
  }
  Harness h;
  struct Reader {
    const std::vector<uint8_t>* data;
    size_t pos;
  } reader{&blob, 0};
  clap_istream is{&reader, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
                    auto* r = static_cast<Reader*>(s->ctx);
                    size_t n = std::min<size_t>(static_cast<size_t>(size), r->data->size() - r->pos);
                    std::memcpy(buffer, r->data->data() + r->pos, n);
                    r->pos += n;
                    return static_cast<int64_t>(n);
                  }};
  CHECK(h.state->load(h.plugin, &is));
  double v = 0;
  CHECK(h.params->get_value(h.plugin, kTrigger, &v) && v == 2);
  CHECK(h.params->get_value(h.plugin, kLength, &v) && v == 0.75);
  CHECK(h.params->get_value(h.plugin, kSync, &v) && v == 1);
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"pass-through when off", testPassThrough},
      {"basic loop", testBasicLoop},
      {"length increase mid-loop", testLengthIncrease},
      {"length decrease before end", testLengthDecreaseBeforeEnd},
      {"length decrease past playhead", testLengthDecreasePastEnd},
      {"retrigger On 1 <-> On 2, then off", testRetriggerAndOff},
      {"sync 1/4 at 120 BPM", testSyncLength},
      {"sync 1/8T at 100 BPM", testSyncTriplet},
      {"sync toggled while looping", testSyncToggleWhileLooping},
      {"10 s max length", testMaxLength},
      {"grow to max after long loop", testGrowAfterLongLoop},
      {"parameter text", testParamText},
      {"sync division order", testSyncDivisionOrder},
      {"state round trip", testStateRoundTrip},
  };
  for (const auto& t : tests) {
    const int before = failures;
    t.fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf(failures ? "\n%d failure(s)\n" : "\nall tests passed\n", failures);
  return failures ? 1 : 0;
}
