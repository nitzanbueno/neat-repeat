#include <clap/clap.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "length.h"
#include "looper.h"

namespace {

using namespace neat;

enum ParamId : clap_id { kTrigger = 0, kLength = 1, kSync = 2, kNumParams = 3 };

constexpr int kNumChannels = 2;
constexpr double kDefaultBpm = 120.0;
constexpr uint32_t kStateVersion = 1;
const char* const kTriggerNames[] = {"Off", "On 1", "On 2"};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_GLITCH,
                                 CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor kDescriptor = {
    CLAP_VERSION_INIT,
    "com.neat-repeat.neat-repeat",
    "Neat Repeat",
    "Neat Repeat",
    "",
    "",
    "",
    "1.0.0",
    "Loops the audio from the moment it is triggered.",
    kFeatures,
};

struct ParamDef {
  const char* name;
  double min, max, def;
  clap_param_info_flags flags;
};

// Indexed by ParamId. The Length default is 100 ms, or 1/16 with Sync on.
const ParamDef kParamDefs[kNumParams] = {
    {"Trigger", 0.0, 2.0, 0.0,
     CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM},
    {"Length", 0.0, 1.0, 1.0 / 3.0, CLAP_PARAM_IS_AUTOMATABLE},
    {"Sync", 0.0, 1.0, 0.0, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED},
};

int triggerFromValue(double v) { return std::clamp(static_cast<int>(std::lround(v)), 0, 2); }
bool syncFromValue(double v) { return v >= 0.5; }

struct Plugin {
  clap_plugin plugin;
  const clap_host* host;
  const clap_host_params* hostParams = nullptr;

  // Written by the audio thread (events) and the main thread (state load).
  std::atomic<double> values[kNumParams];

  // Audio thread only.
  Looper looper;
  double sampleRate = 44100.0;
  double bpm = kDefaultBpm;
  bool hostPlaying = true;  // hosts without a transport count as always playing
  bool appliedPlaying = false;
  int appliedTrigger = 0;
  int64_t appliedLength = 0;
  bool appliedSync = false;

  Plugin(const clap_host* h) : host(h) {
    for (int i = 0; i < kNumParams; ++i) values[i].store(kParamDefs[i].def);
  }

  int64_t currentLengthSamples() const {
    return lengthSamples(values[kLength].load(), syncFromValue(values[kSync].load()), bpm,
                         sampleRate);
  }

  // Brings the looper in line with the current parameter values.
  void applyParams() {
    const bool sync = syncFromValue(values[kSync].load());
    if (sync != appliedSync) {
      appliedSync = sync;
      // The Length display text depends on Sync; ask the host to refresh it.
      host->request_callback(host);
    }

    const int64_t len = currentLengthSamples();
    if (len != appliedLength) {
      looper.setLength(len);
      appliedLength = len;
    }

    // The loop only runs while the transport plays. Any change between
    // Off / On 1 / On 2, or the transport starting, that leaves Trigger on an
    // "on" position starts a fresh loop.
    const int trig = triggerFromValue(values[kTrigger].load());
    if (trig != appliedTrigger || hostPlaying != appliedPlaying) {
      if (trig == 0 || !hostPlaying)
        looper.stop();
      else
        looper.trigger();
      appliedTrigger = trig;
      appliedPlaying = hostPlaying;
    }
  }

  void setParam(clap_id id, double value) {
    if (id >= kNumParams) return;
    values[id].store(std::clamp(value, kParamDefs[id].min, kParamDefs[id].max));
  }

  void updateTransport(const clap_event_transport* t) {
    if ((t->flags & CLAP_TRANSPORT_HAS_TEMPO) && t->tempo > 0.0) bpm = t->tempo;
    hostPlaying = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
  }

  // Returns true if the event may have changed the loop settings.
  bool handleEvent(const clap_event_header* hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
    switch (hdr->type) {
      case CLAP_EVENT_PARAM_VALUE: {
        auto* ev = reinterpret_cast<const clap_event_param_value*>(hdr);
        setParam(ev->param_id, ev->value);
        return true;
      }
      case CLAP_EVENT_TRANSPORT:
        updateTransport(reinterpret_cast<const clap_event_transport*>(hdr));
        return true;
      default:
        return false;
    }
  }

  clap_process_status process(const clap_process* proc) {
    if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1)
      return CLAP_PROCESS_CONTINUE;
    const clap_audio_buffer& in = proc->audio_inputs[0];
    const clap_audio_buffer& out = proc->audio_outputs[0];
    const int channels = static_cast<int>(std::min(in.channel_count, out.channel_count));

    if (proc->transport)
      updateTransport(proc->transport);
    else
      hostPlaying = true;
    applyParams();

    // Split the block at each event so changes are sample-accurate.
    const uint32_t frames = proc->frames_count;
    const uint32_t numEvents = proc->in_events->size(proc->in_events);
    uint32_t nextEvent = 0;
    uint32_t frame = 0;
    while (frame < frames) {
      while (nextEvent < numEvents) {
        const clap_event_header* hdr = proc->in_events->get(proc->in_events, nextEvent);
        if (hdr->time > frame) break;
        if (handleEvent(hdr)) applyParams();
        ++nextEvent;
      }
      uint32_t end = frames;
      if (nextEvent < numEvents)
        end = std::min(frames, proc->in_events->get(proc->in_events, nextEvent)->time);
      looper.process(in.data32, out.data32, channels, frame, end);
      frame = end;
    }
    // Events stamped past the end of the block.
    for (; nextEvent < numEvents; ++nextEvent) {
      if (handleEvent(proc->in_events->get(proc->in_events, nextEvent))) applyParams();
    }
    return CLAP_PROCESS_CONTINUE;
  }

  void formatValue(clap_id id, double value, char* display, uint32_t size) const {
    switch (id) {
      case kTrigger:
        std::snprintf(display, size, "%s", kTriggerNames[triggerFromValue(value)]);
        break;
      case kSync:
        std::snprintf(display, size, "%s", syncFromValue(value) ? "On" : "Off");
        break;
      case kLength:
        if (syncFromValue(values[kSync].load())) {
          std::snprintf(display, size, "%s", kSyncDivisions[syncIndex(value)].name);
        } else {
          const double ms = lengthMs(value);
          if (ms < 100.0)
            std::snprintf(display, size, "%.1f ms", ms);
          else if (ms < 1000.0)
            std::snprintf(display, size, "%.0f ms", ms);
          else
            std::snprintf(display, size, "%.2f s", ms / 1000.0);
        }
        break;
    }
  }
};

bool equalsIgnoreCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
      return false;
  }
  return *a == *b;
}

const char* skipSpaces(const char* s) {
  while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
  return s;
}

bool parseLength(const char* text, double* out) {
  text = skipSpaces(text);
  for (int i = 0; i < kNumSyncDivisions; ++i) {
    if (equalsIgnoreCase(text, kSyncDivisions[i].name)) {
      *out = normFromSyncIndex(i);
      return true;
    }
  }
  char* rest = nullptr;
  double number = std::strtod(text, &rest);
  if (rest == text) return false;
  rest = const_cast<char*>(skipSpaces(rest));
  if (equalsIgnoreCase(rest, "s") || equalsIgnoreCase(rest, "sec")) number *= 1000.0;
  else if (*rest && !equalsIgnoreCase(rest, "ms")) return false;
  *out = normFromMs(number);
  return true;
}

Plugin* self(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- clap.params ------------------------------------------------------------

uint32_t paramsCount(const clap_plugin*) { return kNumParams; }

bool paramsGetInfo(const clap_plugin*, uint32_t index, clap_param_info* info) {
  if (index >= kNumParams) return false;
  const ParamDef& def = kParamDefs[index];
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  info->flags = def.flags;
  info->min_value = def.min;
  info->max_value = def.max;
  info->default_value = def.def;
  std::snprintf(info->name, sizeof(info->name), "%s", def.name);
  return true;
}

bool paramsGetValue(const clap_plugin* p, clap_id id, double* value) {
  if (id >= kNumParams) return false;
  *value = self(p)->values[id].load();
  return true;
}

bool paramsValueToText(const clap_plugin* p, clap_id id, double value, char* display,
                       uint32_t size) {
  if (id >= kNumParams || size == 0) return false;
  self(p)->formatValue(id, value, display, size);
  return true;
}

bool paramsTextToValue(const clap_plugin*, clap_id id, const char* text, double* value) {
  text = skipSpaces(text);
  switch (id) {
    case kTrigger:
      for (int i = 0; i < 3; ++i) {
        if (equalsIgnoreCase(text, kTriggerNames[i])) {
          *value = i;
          return true;
        }
      }
      return false;
    case kSync:
      if (equalsIgnoreCase(text, "on")) *value = 1.0;
      else if (equalsIgnoreCase(text, "off")) *value = 0.0;
      else return false;
      return true;
    case kLength:
      return parseLength(text, value);
    default:
      return false;
  }
}

void paramsFlush(const clap_plugin* p, const clap_input_events* in, const clap_output_events*) {
  Plugin* plug = self(p);
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) plug->handleEvent(in->get(in, i));
}

const clap_plugin_params kParamsExt = {paramsCount,       paramsGetInfo,     paramsGetValue,
                                       paramsValueToText, paramsTextToValue, paramsFlush};

// --- clap.audio-ports -------------------------------------------------------

uint32_t audioPortsCount(const clap_plugin*, bool) { return 1; }

bool audioPortsGet(const clap_plugin*, uint32_t index, bool isInput, clap_audio_port_info* info) {
  if (index != 0) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = 0;
  std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = kNumChannels;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = 0;
  return true;
}

const clap_plugin_audio_ports kAudioPortsExt = {audioPortsCount, audioPortsGet};

// --- clap.state -------------------------------------------------------------

bool writeAll(const clap_ostream* s, const void* data, uint64_t size) {
  auto* bytes = static_cast<const uint8_t*>(data);
  while (size > 0) {
    int64_t n = s->write(s, bytes, size);
    if (n <= 0) return false;
    bytes += n;
    size -= static_cast<uint64_t>(n);
  }
  return true;
}

bool readAll(const clap_istream* s, void* data, uint64_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size > 0) {
    int64_t n = s->read(s, bytes, size);
    if (n <= 0) return false;
    bytes += n;
    size -= static_cast<uint64_t>(n);
  }
  return true;
}

bool stateSave(const clap_plugin* p, const clap_ostream* stream) {
  Plugin* plug = self(p);
  double values[kNumParams];
  for (int i = 0; i < kNumParams; ++i) values[i] = plug->values[i].load();
  return writeAll(stream, &kStateVersion, sizeof(kStateVersion)) &&
         writeAll(stream, values, sizeof(values));
}

bool stateLoad(const clap_plugin* p, const clap_istream* stream) {
  Plugin* plug = self(p);
  uint32_t version = 0;
  double values[kNumParams];
  if (!readAll(stream, &version, sizeof(version)) || version != kStateVersion) return false;
  if (!readAll(stream, values, sizeof(values))) return false;
  for (clap_id i = 0; i < kNumParams; ++i) plug->setParam(i, values[i]);
  if (plug->hostParams) plug->hostParams->rescan(plug->host, CLAP_PARAM_RESCAN_VALUES);
  return true;
}

const clap_plugin_state kStateExt = {stateSave, stateLoad};

// --- clap.plugin ------------------------------------------------------------

bool pluginInit(const clap_plugin* p) {
  Plugin* plug = self(p);
  plug->hostParams =
      static_cast<const clap_host_params*>(plug->host->get_extension(plug->host, CLAP_EXT_PARAMS));
  return true;
}

void pluginDestroy(const clap_plugin* p) { delete self(p); }

bool pluginActivate(const clap_plugin* p, double sampleRate, uint32_t, uint32_t) {
  Plugin* plug = self(p);
  plug->sampleRate = sampleRate;
  plug->looper.prepare(kNumChannels, maxLoopSamples(sampleRate));
  plug->appliedTrigger = 0;
  plug->hostPlaying = true;
  plug->appliedPlaying = false;
  plug->appliedSync = syncFromValue(plug->values[kSync].load());
  plug->appliedLength = plug->currentLengthSamples();
  plug->looper.setLength(plug->appliedLength);
  return true;
}

void pluginDeactivate(const clap_plugin*) {}
bool pluginStartProcessing(const clap_plugin*) { return true; }
void pluginStopProcessing(const clap_plugin*) {}

void pluginReset(const clap_plugin* p) {
  Plugin* plug = self(p);
  plug->looper.stop();
  plug->appliedTrigger = 0;
}

clap_process_status pluginProcess(const clap_plugin* p, const clap_process* proc) {
  return self(p)->process(proc);
}

const void* pluginGetExtension(const clap_plugin*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParamsExt;
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPortsExt;
  if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kStateExt;
  return nullptr;
}

void pluginOnMainThread(const clap_plugin* p) {
  Plugin* plug = self(p);
  if (plug->hostParams) plug->hostParams->rescan(plug->host, CLAP_PARAM_RESCAN_TEXT);
}

// --- factory and entry ------------------------------------------------------

uint32_t factoryGetPluginCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryGetPluginDescriptor(const clap_plugin_factory*,
                                                         uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin* factoryCreatePlugin(const clap_plugin_factory*, const clap_host* host,
                                       const char* pluginId) {
  if (!clap_version_is_compatible(host->clap_version) ||
      std::strcmp(pluginId, kDescriptor.id) != 0)
    return nullptr;

  Plugin* plug = new (std::nothrow) Plugin(host);
  if (!plug) return nullptr;
  plug->plugin.desc = &kDescriptor;
  plug->plugin.plugin_data = plug;
  plug->plugin.init = pluginInit;
  plug->plugin.destroy = pluginDestroy;
  plug->plugin.activate = pluginActivate;
  plug->plugin.deactivate = pluginDeactivate;
  plug->plugin.start_processing = pluginStartProcessing;
  plug->plugin.stop_processing = pluginStopProcessing;
  plug->plugin.reset = pluginReset;
  plug->plugin.process = pluginProcess;
  plug->plugin.get_extension = pluginGetExtension;
  plug->plugin.on_main_thread = pluginOnMainThread;
  return &plug->plugin;
}

const clap_plugin_factory kFactory = {factoryGetPluginCount, factoryGetPluginDescriptor,
                                      factoryCreatePlugin};

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* factoryId) {
  return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                             entryDeinit, entryGetFactory};
