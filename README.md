Sabrewing
=========

This is an algorithmic profiler for x86-64 systems running Windows or Linux. It's a very early
prototype and not ready for serious use.

Screenshot
----------

![screenshot](screenshots/screenshot.webp)

Installation
------------

To build on Winows, install MSVC, and from a Developer Command Prompt run:

    > .\build.bat

To build and run on Linux:

    $ make
    $ cd build
    $ ./sabrewing

Profiling Tips
--------------

It can be challenging to get consistent results when profiling modern hardware, but there are a few
ways to improve your odds.

- Poor repeatability is often caused by dynamic frequency scaling: The CPU will transition to a
  higher "boost" frequency shortly after the workload begins, but remain there only temporarily due
  to constraints on energy usage and/or heat dissipation. Frequency scaling tends to be especially
  agressive on mobile devices. You can temporarily disable it (to some extent) either in your
  BIOS/UEFI settings or through your operating system. There may be multiple features that require
  disabling; look for phrases like "processor power management", "CPU throttling", and "performance
  boost mode". On Windows, sometimes these settings are hidden until you modify some associated
  registry values.

- Disable items like "CPU Clock Spread Spectrum" and "SB Clock Spread Spectrum" in your motherboard
  settings. These feature deliberately introduce jitter into the clock, modulating the frequency by
  as much as 1%. There is typically no harm in leaving spread spectrum disabled (its main purpose is
  to reduce narrow-band EM emissions for regulatory compliance).

- To minimize process pre-empting by the operating system, it's best to shut down other applications
  such as web browsers, especially if you're profiling multi-threaded code that can use all
  available CPU cores.

- Try experimenting with the timing method settings in the profiler.
