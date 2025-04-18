Sabrewing
=========

An algorithmic profiler.

This is a very early prototype and not ready for serious use.

Screenshot
----------

![screenshot](screenshots/screenshot.webp)

Installation
------------

To build on Winows, install MSVC, and from a Developer Command Prompt run:

    > .\build.bat


Profiling Tips
--------------

- If you're profiling on a laptop, you may get inconsistent results because of aggressive CPU
  dynamic frequency scaling. For instance, often the CPU will switch to a higher "boost" frequency
  shortly after the workload begins, but remain there only temporarily due to constraints on energy
  usage and/or heat dissipation. To work around this, you can disable frequency scaling. This can be
  done either from your BIOS/UEFI settings or through your operating system. There may be multiple
  features that require disabling; look for phrases like "processor power management", "CPU
  throttling", and "performance boost mode". Note that disabling frequency scaling will degrade your
  system's performance.

- To minimize process pre-empting, it's best to shut down other applications such as web browsers.

- If you're getting inconsistent results, try changing the timing method settings.
