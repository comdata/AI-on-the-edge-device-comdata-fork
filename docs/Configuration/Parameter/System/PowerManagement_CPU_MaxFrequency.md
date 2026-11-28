# Parameter: Frequency

|                   | WebUI               | REST API
|:---               |:---                 |:----
| Parameter Name    | Frequency       | maxfrequency
| Default Value     | `160`               | `160`
| Input Options     | `80`<br>`160`<br>`240`      | `80`<br>`160`<br>`240` 
| Unit              | Mhz                 | Mhz


!!! Warning
    This is an **Expert Parameter**! Only change it if you understand what it does!  


## Description

Set the maximum CPU frequency.

Dynamic frequency scaling is enabled: the CPU runs at the configured maximum while it is busy
and drops to 80 MHz when idle. 80 MHz is the lower limit, because WiFi and the external PSRAM
require at least 80 MHz. Choosing `80` disables the scaling and fixes the CPU at 80 MHz.

A lower maximum reduces power consumption and the SoC temperature, but lengthens image
processing (digit recognition takes roughly twice as long at 80 MHz).


!!! Note
    To apply this parameter a device reboot is required.


!!! Warning
    Board ESP32CAM: **It is strongly recommended to use 160Mhz**<br>
    Setting it to 240Mhz will lead to a faster processing, but it requests a stronger power supply.
    Biggest drawback: Depending on the manufacturing quality of your device, system might generally run more
    unstable and WIFI performance could be bad / unstable.
