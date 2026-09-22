# CPU Monitor basic example

Build this standalone example for any installed ESP-IDF target; it detects the
active FreeRTOS core count at compile time and never assumes Core 1 exists.

```sh
source /Users/shuipi/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
```

After the five default warmup samples, the serial log prints each available core
and the mean total load once per second. A CPU-bound task pinned to an available
core should raise that core's estimate toward 100%.
