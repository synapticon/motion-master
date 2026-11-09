# p1535

ESP32 firmware that switches the output of a PeakTech P 1535 power supply on and off over HTTP.

It is an ESP-IDF project. It does not build with the CMake presets of this repository, and
`hil/CMakeLists.txt` does not include it.

```bash
./setup.sh       # once per computer
./configure.sh   # set the WiFi password
./run.sh         # build, flash, and open the serial monitor
./power.sh off   # or on
```

[P 1535 Remote Control](../../docs/P1535_REMOTE_CONTROL.md) covers each script, the configuration, and
the endpoints.
