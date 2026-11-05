_This readme was written by a human._

# Neat Repeat

This plugin is a very bare-bones clone of Ableton's Beat Repeat.  
It repeats the incoming audio when triggered.  
FL Studio has Transporter, Ableton has Beat Repeat, and Bitwig has nothing.  
I tried making a version of it in the grid, but it worked poorly, so I opted to making a quick plugin.

## AI Usage

This plugin is **entirely** AI-generated.  
I have only specified the requirements, and I have tested it myself for my own music to make sure it works.  
This is a very minimalistic plugin with no real decisions about DSP, and that's the point - it's a tiny tool missing from Bitwig to add to its arsenal.

## Usage

The plugin has no UI, and is designed mostly to be automated on Bitwig.

The plugin has three parameters:

- **Trigger**: When this parameter is turned on, the beat starts repeating immediately.
    - Notice that there are two "on" triggers - "On 1" and "On 2". They do the exact same thing, but switching from one to the other causes a retrigger. This way, there's no need to have a tiny blip to stop repeating before retriggering.
- **Time**: This parameter controls the repeat time. It's either in milliseconds or beats, depending on the sync parameter.
  The range is between 10ms-10s, or between 1/64th and 1 bar (with dotted and triplet times included in the middle).
- **Sync**: This parameter controls whether the time parameter is synced to the BPM or not.
