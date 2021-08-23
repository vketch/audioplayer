# An Audio Player for mbed-os

This library can be used to play wave audio on mbed-os devices over PWM.

## Example usage
Play music on NUCLEO_F412ZG. Speaker is connected to PA_0.

```
#include "mbed.h"
#include "SDBlockDevice.h"
#include "FATFileSystem.h"
#include "AudioPlayer.h"

// PWM
PwmOut speaker(PA_0);

// Connection for SD card
SDBlockDevice   sd(PB_5, PB_4, PB_3, D2);
FATFileSystem fs("sd", &sd);

AudioPlayer player(&speaker);

int main()
{
    // set PWN frequency
    speaker.period_us(40);

    // Set the maximum speed so it can keep up with audio
    sd.frequency(25000000);

    File file(&fs, "sample.wav");
    player.play(&file);

}

```
