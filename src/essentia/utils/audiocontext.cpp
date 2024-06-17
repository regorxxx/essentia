/*
 * Copyright (C) 2006-2021  Music Technology Group - Universitat Pompeu Fabra
 *
 * This file is part of Essentia
 *
 * Essentia is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation (FSF), either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the Affero GNU General Public License
 * version 3 along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "audiocontext.h"
#include <iostream> // for warning cout

using namespace std;
using namespace essentia;

AudioContext::AudioContext()
  : _isOpen(false), _avStream(0), _muxCtx(0), _codecCtx(0),
    _inputBufSize(0), _buffer(0), _convertCtxAv(0) {
  av_log_set_level(AV_LOG_VERBOSE);
  //av_log_set_level(AV_LOG_QUIET);
  
  if (sizeof(float) != av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT)) {
    throw EssentiaException("Unsupported float size");
  }
}


int AudioContext::create(const std::string& filename,
                         const std::string& format,
                         int nChannels, int sampleRate, int bitrate) {
  if (_muxCtx != 0) close();

  _filename = filename;
  /*
  const AVOutputFormat* av_output_format = av_guess_format(format.c_str(), 0, 0);
  if (!av_output_format) {
    throw EssentiaException("Could not find a suitable output format for \"", filename, "\"");
  }
  if (format != av_output_format->name) {
    E_WARNING("Essentia is using a different format than the one supplied. Format used is " << av_output_format->name);
  }

  _muxCtx = avformat_alloc_context();
  if (!_muxCtx) throw EssentiaException("Could not allocate the format context");

  _muxCtx->oformat = av_output_format;
  */

  if (avformat_alloc_output_context2(&_muxCtx, NULL, NULL, filename.c_str()) < 0)
      throw EssentiaException("Could not allocate the format context");

  //_muxCtx->url = av_strdup(filename.c_str());

  const AVOutputFormat* av_output_format = _muxCtx->oformat;

  // Find encoder
  av_log_set_level(AV_LOG_VERBOSE);
  const AVCodec* audioCodec = avcodec_find_encoder(av_output_format->audio_codec);
  if (!audioCodec) {
    throw EssentiaException("Could not find an encoder");
  }

  // Create audio stream
  _avStream = avformat_new_stream(_muxCtx, NULL);
  if (!_avStream) {
    throw EssentiaException("Could not allocate stream");
  }
  //_avStream->id = 1; // necessary? found here: http://sgros.blogspot.com.es/2013/01/deprecated-functions-in-ffmpeg-library.html

  _codecCtx = avcodec_alloc_context3(audioCodec);
  if (!_codecCtx) {
    throw EssentiaException("Could not allocate an encoding context");
  }
  // Set up codec:
  _codecCtx->codec_type     = AVMEDIA_TYPE_AUDIO;
  _codecCtx->bit_rate       = bitrate;
  _codecCtx->sample_rate    = sampleRate;
#if LIBAVCODEC_VERSION_MAJOR < 59
  _codecCtx->channels = nChannels;
  _codecCtx->channel_layout = av_get_default_channel_layout(nChannels);
#else
  av_channel_layout_default(&_codecCtx->ch_layout, nChannels);
#endif

  switch (_codecCtx->codec_id) {
    case AV_CODEC_ID_VORBIS:
      _codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
      break;
    case AV_CODEC_ID_MP3:
      _codecCtx->sample_fmt = AV_SAMPLE_FMT_S16P;
      break;
    default:
      _codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
  }

  // Check if the hardcoded sample format is supported by the codec
  if (audioCodec->sample_fmts) {
    const enum AVSampleFormat* p = audioCodec->sample_fmts;
    while (*p != AV_SAMPLE_FMT_NONE) {
      if (*p == _codecCtx->sample_fmt) break;
      p++;
    }
    if (*p == AV_SAMPLE_FMT_NONE) {
      // Not supported --> use the first one in the list as default?
      // _codecCtx->sample_fmt = audioCodec->sample_fmts[0];
      ostringstream msg;  
      msg << "AudioWriter: Could not open codec \"" << audioCodec->long_name << "\" for "
          << format << " files: sample format " << av_get_sample_fmt_name(_codecCtx->sample_fmt) << " is not supported";
      throw EssentiaException(msg);
    }
  }

  // Open codec and store it in _codecCtx. 
  int result = avcodec_open2(_codecCtx, audioCodec, NULL);
  if (result < 0) {
    char errstring[1204];
    av_strerror(result, errstring, sizeof(errstring));

    ostringstream msg;  
    msg << "AudioWriter: Could not open codec \"" << audioCodec->long_name << "\" for " << format << " files: " << errstring;
    throw EssentiaException(msg);
  }

  switch (_codecCtx->codec_id) {
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
      // PCM codecs do not provide frame size in samples, use 4096 bytes on input
#if LIBAVCODEC_VERSION_MAJOR < 59
        _codecCtx->frame_size = 4096 / _codecCtx->channels / av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
#else
        _codecCtx->frame_size = 4096 / _codecCtx->ch_layout.nb_channels / av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
#endif
      break;

    //case AV_CODEC_ID_FLAC:
    //case AV_CODEC_ID_VORBIS:
    //  break;

    default:
      if (_codecCtx->frame_size <= 1) {
        throw EssentiaException("Do not know how to encode given format: ", format);
      }
  }

  if (avcodec_parameters_from_context(_avStream->codecpar, _codecCtx) < 0) {
      throw EssentiaException("Could not initialize stream parameters");
  }

  // Allocate input audio FLT buffer
  _inputBufSize = av_samples_get_buffer_size(NULL,
#if LIBAVCODEC_VERSION_MAJOR < 59
      _codecCtx->channels,
#else
      _codecCtx->ch_layout.nb_channels,
#endif
      _codecCtx->frame_size, AV_SAMPLE_FMT_FLT, 0);

  _buffer = (float*)av_malloc(_inputBufSize);

  _packet = av_packet_alloc();
  if (!_packet) {
      throw EssentiaException("Error allocating AVPacket");
  }

  _frame = av_frame_alloc();
  if (!_frame) {
      throw EssentiaException("Error allocating AVFrame");
  }

  // Configure sample format conversion
  //E_DEBUG(EAlgorithm, "AudioContext: using sample format conversion from libswresample");
  _convertCtxAv = swr_alloc();
  if (!_convertCtxAv) {
      throw EssentiaException("AudioLoader: Could not allocate swresample context");
  }

  /* set options */
#if LIBAVCODEC_VERSION_MAJOR < 59
  av_opt_set_int(_convertCtxAv, "in_channel_layout", _codecCtx->channel_layout, 0);
  av_opt_set_int(_convertCtxAv, "in_sample_rate", _codecCtx->sample_rate, 0);
  av_opt_set_int(_convertCtxAv, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

  av_opt_set_int(_convertCtxAv, "out_channel_layout", _codecCtx->channel_layout, 0);
  av_opt_set_int(_convertCtxAv, "out_sample_rate", _codecCtx->sample_rate, 0);
  av_opt_set_int(_convertCtxAv, "out_sample_fmt", _codecCtx->sample_fmt, 0);
#else
  av_opt_set_chlayout(_convertCtxAv, "in_chlayout", &_codecCtx->ch_layout, 0);
  av_opt_set_int(_convertCtxAv, "in_sample_rate", _codecCtx->sample_rate, 0);
  av_opt_set_sample_fmt(_convertCtxAv, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

  av_opt_set_chlayout(_convertCtxAv, "out_chlayout", &_codecCtx->ch_layout, 0);
  av_opt_set_int(_convertCtxAv, "out_sample_rate", _codecCtx->sample_rate, 0);
  av_opt_set_sample_fmt(_convertCtxAv, "out_sample_fmt", _codecCtx->sample_fmt, 0);
#endif

  if (swr_init(_convertCtxAv) < 0) {
      throw EssentiaException("AudioLoader: Could not initialize swresample context");
  }

  if (swr_is_initialized(_convertCtxAv) == 0) {
      throw EssentiaException("AudioLoader: Could not initialize swresample context");
  }

  return _codecCtx->frame_size;
}


void AudioContext::open() {
  if (_isOpen) return;

  if (!_muxCtx) throw EssentiaException("Trying to open an audio file that has not been created yet or has been closed");

  // Open output file
  if (avio_open(&_muxCtx->pb, _filename.c_str(), AVIO_FLAG_WRITE) < 0) {
    throw EssentiaException("Could not open \"", _filename, "\"");
  }

  avformat_write_header(_muxCtx, /* AVDictionary **options */ NULL);
  _isOpen = true;
}


void AudioContext::close() {
  if (!_muxCtx) return;

  // Close output file
  if (_isOpen) {
    writeEOF();

    // Write trailer to the end of the file
    av_write_trailer(_muxCtx);

    avio_close(_muxCtx->pb);
  }

  avcodec_free_context(&_codecCtx);
  avformat_free_context(_muxCtx);

  av_freep(&_buffer);
  av_freep(&_convert_buffer[0]);

  av_packet_free(&_packet);
  av_frame_free(&_frame);

  //av_freep(&_avStream);

  // TODO: need those assignments?
  _muxCtx = 0;
  _avStream = 0;
  _codecCtx = 0;
  _buffer = 0;

  if (_convertCtxAv) {
    swr_close(_convertCtxAv);
    swr_free(&_convertCtxAv);
  }

  _isOpen = false;
}


void AudioContext::write(const vector<StereoSample>& stereoData) {
#if LIBAVCODEC_VERSION_MAJOR < 59
    int channels = _codecCtx->channels;
#else
    int channels = _codecCtx->ch_layout.nb_channels;
#endif
  if (channels != 2) {
    throw EssentiaException("Trying to write stereo audio data to an audio file with ", channels, " channels");
  }

  int dsize = (int)stereoData.size();
  
  if (dsize > _codecCtx->frame_size) {
    // AudioWriter sets up correct buffer sizes in accordance to what 
    // AudioContext:create() returns. Nevertheless, double-check here.
    ostringstream msg;
    msg << "Audio frame size " << _codecCtx->frame_size << 
           " is not sufficient to store " << dsize << " samples";
    throw EssentiaException(msg);
  }

  for (int i=0; i<dsize; ++i) {
    _buffer[2*i] = (float) stereoData[i].left();
    _buffer[2*i+1] = (float) stereoData[i].right();
  }

  encodePacket(dsize);
}


void AudioContext::write(const vector<AudioSample>& monoData) {
#if LIBAVCODEC_VERSION_MAJOR < 59
    int channels = _codecCtx->channels;
#else
    int channels = _codecCtx->ch_layout.nb_channels;
#endif
  if (channels != 1) {
    throw EssentiaException("Trying to write mono audio data to an audio file with ", channels, " channels");
  }

  int dsize = (int)monoData.size();
  if (dsize > _codecCtx->frame_size) {
    // The same as for stereoData version of write()
    ostringstream msg;
    msg << "Audio frame size " << _codecCtx->frame_size << 
           " is not sufficient to store " << dsize << " samples";
    throw EssentiaException(msg);
  }

  for (int i=0; i<dsize; ++i) _buffer[i] = (float) monoData[i];

  encodePacket(dsize);
}


void AudioContext::encodePacket(int size) {

  int tmp_fs = _codecCtx->frame_size;

  if (size < _codecCtx->frame_size) {
    _codecCtx->frame_size = size;
  }
  else if (size > _codecCtx->frame_size) {
    // input audio vector does not fit into the codec's buffer
    throw EssentiaException("AudioLoader: Input audio segment is larger than the codec's frame size");
  }

  /* allocate the data buffers */
  int num_out_samples = swr_get_out_samples(_convertCtxAv, size);

  int linesize;
  if (num_out_samples > _convert_buffer_size) {
      if (_convert_buffer)
          av_freep(&_convert_buffer[0]);
      _convert_buffer_size = num_out_samples;
      if (av_samples_alloc_array_and_samples(&_convert_buffer, &linesize,
#if LIBAVCODEC_VERSION_MAJOR < 59
          _codecCtx->channels,
#else
          _codecCtx->ch_layout.nb_channels,
#endif
          num_out_samples, _codecCtx->sample_fmt, 0) < 0) {
          throw EssentiaException("Could not allocate output buffer for sample format conversion");
      }
  }
 
  // convert sample format to the one required by codec
  int written = swr_convert(_convertCtxAv,
      _convert_buffer,
      num_out_samples,
      (const uint8_t**) &_buffer,
      size);

  if (written < size) {
    // The same as in AudioLoader. There may be data remaining in the internal 
    // FIFO buffer to get this data: call swr_convert() with NULL input
    // But we just throw exception instead.
    ostringstream msg;
    msg << "AudioLoader: Incomplete format conversion (some samples missing)"
        << " from " << av_get_sample_fmt_name(AV_SAMPLE_FMT_FLT)
        << " to "   << av_get_sample_fmt_name(_codecCtx->sample_fmt);
    throw EssentiaException(msg);
  }

  _frame->nb_samples = _codecCtx->frame_size;
  _frame->format = _codecCtx->sample_fmt;

#if LIBAVCODEC_VERSION_MAJOR < 59
  _frame->channel_layout = _codecCtx->channel_layout;
#else
  int ret = av_channel_layout_copy(&_frame->ch_layout, &_codecCtx->ch_layout);
  if (ret < 0) {
      char errstring[1204];
      av_strerror(ret, errstring, sizeof(errstring));
      ostringstream msg;
      msg << "Error while encoding audio frame: " << errstring;
      throw EssentiaException(msg);
  }
#endif

#if LIBAVCODEC_VERSION_MAJOR < 59
  int channels = _codecCtx->channels;
#else
  int channels = _codecCtx->ch_layout.nb_channels;
#endif
  int buffer_size = av_samples_get_buffer_size(NULL, channels, size, AV_SAMPLE_FMT_FLT, 0);

  int result = avcodec_fill_audio_frame(_frame, channels, _codecCtx->sample_fmt,
                                        _convert_buffer[0], buffer_size, 0);
  if (result < 0) {
    char errstring[1204];
    av_strerror(result, errstring, sizeof(errstring));
    ostringstream msg;
    msg << "Could not setup audio frame: " << errstring;
    throw EssentiaException(msg);
  }

  /* send the frame for encoding */
  result = avcodec_send_frame(_codecCtx, _frame);
  if (result < 0) {
      char errstring[1204];
      av_strerror(result, errstring, sizeof(errstring));
      throw EssentiaException("Error sending the frame to the encoder");
  }

  // Set the packet data and size so that it is recognized as being empty.
  _packet->data = NULL;
  _packet->size = 0;

  /* read all the available output packets */
  while (result >= 0) {
      result = avcodec_receive_packet(_codecCtx, _packet);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
          goto cleanup;
      else if (result < 0) {
          throw EssentiaException("Error encoding audio frame");
      }
      if (av_write_frame(_muxCtx, _packet) != 0) {
          throw EssentiaException("Error while writing audio frame");
      }
      av_packet_unref(_packet);
  }

  _codecCtx->frame_size = tmp_fs;
cleanup:
  av_frame_unref(_frame);
  av_packet_unref(_packet);
}

void AudioContext::writeEOF() { 
  AVPacket* packet = av_packet_alloc();
  // Set the packet data and size so that it is recognized as being empty.
  packet->data = NULL;
  packet->size = 0;

  /* send the frame for encoding */
  int ret = avcodec_send_frame(_codecCtx, NULL);
  if (ret < 0) {
    throw EssentiaException("Error sending delayed frame to the encoder");
  }

  /* read all the available output packets */
  while (ret >= 0) {
    ret = avcodec_receive_packet(_codecCtx, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;
    else if (ret < 0) {
      throw EssentiaException("Error encoding delayed audio frame");
    }
    if (av_write_frame(_muxCtx, packet) != 0) {
      throw EssentiaException("Error while writing delayed audio frame");
    }
    av_packet_unref(packet);
  }
}
