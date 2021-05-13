/* mbed Microcontroller Library
 * Copyright (c) 2017-2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AudioPlayer.h"
#include "WaveAudioStream.h"
#include <chrono>

#define AUDIO_BUF_SIZE      (1 * 1024)
#define MIN_FREQUENCY_HZ    8000

#define FLAG_BUF_FREE       (1 << 0)
#define FLAG_END            (1 << 1)
#define FLAG_STOP           (1 << 2)

#define ERROR_FREQUENCY_HZ  1000
#define ERROR_DURATION_MS   1000
#define ERROR_COUNT         3
#define ERROR_LOUDNESS      (1 << 13)

struct audio_buffer_t {
    audio_buffer_t *next;
    uint8_t *data;
    uint32_t size;
    uint32_t max_size;
};

using namespace std::chrono;

static void enque_list(audio_buffer_t **list, audio_buffer_t *element)
{
    core_util_critical_section_enter();
    element->next = NULL;
    if (NULL == *list) {
        *list = element;
    } else {
        audio_buffer_t *cur = *list;
        while (cur->next != NULL) {
            cur = cur->next;
        }
        cur->next = element;
    }
    core_util_critical_section_exit();
}

static audio_buffer_t *deque_list(audio_buffer_t **list)
{
    core_util_critical_section_enter();
    audio_buffer_t *front = *list;
    if (front != NULL) {
        *list = front->next;
        front->next = NULL;
    }
    core_util_critical_section_exit();
    return front;
}

static void reduce_to_8(uint8_t *dest, uint8_t *source, uint8_t channels, uint32_t count, uint32_t sample_size)
{
    uint32_t pos = sample_size - 1;
    for (uint32_t i = 0; i < count; i++) {
        int16_t value = 0;
        for (uint32_t j = 0; j < channels; j++) {
            if (1 == sample_size) {
                value += source[pos];
            } else {
                value += (int8_t)source[pos];
            }
            pos += sample_size;
        }
        dest[i] = value / channels;
        if (sample_size != 1) {
            dest[i] += 128;
        }
    }

}


static microseconds div_round(uint32_t dividend, uint32_t divisior)
{
    return  microseconds ((dividend + divisior / 2) / divisior);
}

bool AudioPlayer::_load_next_buf(AudioStream *stream)
{

    audio_buffer_t *buf = deque_list(&_free_bufs);
    MBED_ASSERT(buf != NULL);

    // read audio data from stream
    int result = stream->read(buf->data, buf->max_size);
    if (result < 0) {
        enque_list(&_free_bufs, buf);
        return false;
    }

    buf->size = result;
    enque_list(&_used_bufs, buf);

    return true;
}

AudioPlayer::AudioPlayer(PwmOut *mono):
        _frequency(0), _mono(mono), _used_bufs(0), _free_bufs(0),
        _cur_buf(0), _cur_pos(0), _error_count(0)
{
    audio_buffer_t *ping = new audio_buffer_t();
    ping->data = new uint8_t[AUDIO_BUF_SIZE]();
    ping->max_size = AUDIO_BUF_SIZE;
    enque_list(&_free_bufs, ping);

    audio_buffer_t *pong = new audio_buffer_t();
    pong->data = new uint8_t[AUDIO_BUF_SIZE]();
    pong->max_size = AUDIO_BUF_SIZE;
    enque_list(&_free_bufs, pong);
}

AudioPlayer::~AudioPlayer()
{
    _ticker.detach();

    audio_buffer_t *entry = deque_list(&_free_bufs);
    while (entry != NULL) {
        delete[] entry->next;
        delete entry;
        entry = deque_list(&_free_bufs);
    }

    entry = deque_list(&_used_bufs);
    while (entry != NULL) {
        delete[] entry->next;
        delete entry;
        entry = deque_list(&_used_bufs);
    }
}

void AudioPlayer::_ticker_handler()
{
    if (_error_count) {
        // Play error tone
        uint16_t tone = _error_tone();
        _mono->write(float(tone/65536));
        return;
    }

    if (NULL == _cur_buf) {
        _cur_buf = deque_list(&_used_bufs);
        _cur_pos = 0;
        if (NULL == _cur_buf) {
            // No data so turn off audio
            _flags.set(FLAG_END);
            _frequency = 0;
            _ticker.detach();
            return;
        }
    }

    // Write audio data
    float  value = *(uint16_t *)(_cur_buf->data+_cur_pos)/65536.0;
    _mono->write(value);
    _cur_pos += 2;
    if (_cur_pos >= _cur_buf->size) {
        enque_list(&_free_bufs, _cur_buf);
        _cur_buf = NULL;
        _flags.set(FLAG_BUF_FREE);
    }
}

uint16_t AudioPlayer::_error_tone()
{
    if (_error_count) {
        _error_count--;
    }
    const uint32_t period = (_frequency / 1000) * (ERROR_DURATION_MS / ERROR_COUNT);
    const uint32_t period_on = period / 2;

    if (_error_count % period > period_on) {
        // Tone off
        return 1 << 15;
    } else {
        // Tone on
        const uint32_t freq_period = _frequency / ERROR_FREQUENCY_HZ;
        const uint32_t freq_period_low = freq_period / 2;
        if (_error_count % freq_period < freq_period_low) {
            return (1 << 15) + ERROR_LOUDNESS;
        } else {
            return (1 << 15) - ERROR_LOUDNESS;
        }

    }
}

bool AudioPlayer::play(File *file)
{
    MBED_ASSERT(_flags.get() == 0);

    WaveAudioStream stream(file);
    if (!stream.get_valid()) {
        return false;
    }

    bool ret = true;
    _load_next_buf(&stream);
    _load_next_buf(&stream);
    _frequency =  stream.get_sample_rate();
    microseconds tick_us = div_round(1000000, _frequency);
    _ticker.attach(Callback<void()>(this, &AudioPlayer::_ticker_handler), tick_us);

    do {
        uint32_t flags = _flags.wait_any(FLAG_BUF_FREE | FLAG_END | FLAG_STOP);
        if (flags & FLAG_END) {
            debug("Error on playing audio\n");
            play_error();
            ret = false;
            break;
        }else  if (flags & FLAG_STOP) {
            debug("Stop playing audio\n");
            ret = true;
            break;
        }
    } while (_load_next_buf(&stream));
    _flags.wait_any(FLAG_END | FLAG_STOP);
    _flags.clear();

    return ret;
}

void AudioPlayer::stop()
{
    _flags.set(FLAG_STOP);
}

void AudioPlayer::play_error()
{
    core_util_critical_section_enter();
    if (_frequency == 0) {
        _frequency = MIN_FREQUENCY_HZ;
        microseconds tick_us = div_round(1000000, _frequency);
        _ticker.attach(Callback<void()>(this, &AudioPlayer::_ticker_handler), tick_us);
    }
    _error_count = _frequency / 1000 * ERROR_DURATION_MS;
    core_util_critical_section_exit();
}
