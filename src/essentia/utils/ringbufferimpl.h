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

#ifndef ESSENTIA_STREAMING_RINGBUFFERIMPL_H
#define ESSENTIA_STREAMING_RINGBUFFERIMPL_H

#include "atomic.h"

#ifdef CPP_11

#include <mutex>
#include <condition_variable>

#elif defined(OS_WIN32)

#include <windows.h>

class Condition {
 protected:
  int waitersCount;
  CRITICAL_SECTION conditionLock;
  CRITICAL_SECTION waitersCountLock;
  HANDLE event;

 public:
  Condition() {
    InitializeCriticalSection(&conditionLock);
    InitializeCriticalSection(&waitersCountLock);
    event = CreateEvent (NULL,  // no security
                         FALSE, // auto-reset event
                         FALSE, // non-signaled initially
                         NULL); // unnamed
    waitersCount = 0;
  }

  void lock()   { EnterCriticalSection(&conditionLock); }
  void unlock() { LeaveCriticalSection(&conditionLock); }

  void wait() {
    EnterCriticalSection(&waitersCountLock);
    waitersCount++;
    LeaveCriticalSection(&waitersCountLock);

    LeaveCriticalSection(&conditionLock);

    int result = WaitForSingleObject(event, INFINITE);

    EnterCriticalSection(&waitersCountLock);
    waitersCount--;
    LeaveCriticalSection(&waitersCountLock);

    EnterCriticalSection(&conditionLock);
  }

  void signal() {
    // Avoid race conditions.
    EnterCriticalSection(&waitersCountLock);
    bool haveWaiters = waitersCount > 0;
    LeaveCriticalSection(&waitersCountLock);

    if (haveWaiters)
      SetEvent(event);
  }
};


#else // OS_WIN32

#include <pthread.h>

class Condition {
 protected:
  pthread_mutex_t pthreadMutex;
  pthread_cond_t pthreadCondition;

 public:
  Condition() {
    pthread_mutex_init(&pthreadMutex,0);
    pthread_cond_init(&pthreadCondition,0);
  }

  void lock()   { pthread_mutex_lock(&pthreadMutex); }
  void unlock() { pthread_mutex_unlock(&pthreadMutex); }
  void wait()   { pthread_cond_wait(&pthreadCondition, &pthreadMutex); }
  void signal() { pthread_cond_signal(&pthreadCondition); }

};


#endif // OS_WIN32


namespace essentia {
namespace streaming {

class RingBufferImpl {
 public:
  int _bufferSize;

  int _writeIndex;
  int _readIndex;

  Atomic _available;
  Atomic _space;

  Real* _buffer;

#ifdef CPP_11
  std::condition_variable condition;
  std::mutex mutex;
#else
  Condition condition;
#endif

  // whether to wait for space (to add data to the buffer)
  // or for availability of data (when reading data from the buffer)
  enum WaitingCondition
  {
    kAvailable, kSpace
  } _waitingCondition;

  RingBufferImpl(WaitingCondition c, int bufferSize)
  : _bufferSize(bufferSize)
  , _writeIndex(0)
  , _readIndex(0)
  , _available(0)
  , _space(_bufferSize)
  , _waitingCondition(c)
  {
    _buffer = new Real[_bufferSize];
  }

  ~RingBufferImpl()
  {
    delete [] _buffer;
  }

  void reset() {
    _writeIndex = 0;
    _readIndex = 0;
    _available = 0;
    _space = _bufferSize;
    delete[] _buffer;
    _buffer = new Real[_bufferSize];
  }

  void waitAvailable(void)
  {
    // this function should only be called if the waiting condition
    // has been set accordingly
    assert(_waitingCondition == kAvailable);

#ifdef CPP_11
    std::unique_lock<std::mutex> lock{mutex};
    condition.wait(lock, [&] { return _available > 0; });
    lock.unlock();
#else
    condition.lock();
    while (_available == 0)
    {
      condition.wait();
    }
    condition.unlock();
#endif
  }

  void waitSpace(void)
  {
    // this function should only be called if the waiting condition
    // has been set accordingly
    assert(_waitingCondition == kSpace);

#ifdef CPP_11
    std::unique_lock<std::mutex> lock{ mutex };
    condition.wait(lock, [&] { return _space > 0; });
    lock.unlock();
#else
    condition.lock();
    while (_space == 0)
    {
      condition.wait();
    }
    condition.unlock();
#endif
  }

  int add(const Real* inputData, int inputSize)
  {
    int size = _space;
    if (size > inputSize) size = inputSize;

    if (_writeIndex + size > _bufferSize)
    {
      int n = _bufferSize - _writeIndex;
      memcpy( &_buffer[_writeIndex], inputData, n * sizeof(AudioSample));
      memcpy( _buffer, &inputData[n], (size - n)*sizeof(AudioSample));
      _writeIndex = (size - n);
    } else {
      memcpy( &_buffer[_writeIndex], inputData, size * sizeof(AudioSample));
      _writeIndex += size;
    }
    _space -=  size;
    _available += size;

    // the thread that is using this ringbuffer will be waiting for
    // data to become available - typically the essentia-part from
    // a RingBufferInput. we signal the waiting condition here
#ifdef CPP_11
    std::lock_guard<std::mutex> lock{ mutex };
    if (_waitingCondition == kAvailable)
    {
        condition.notify_one();
    }
#else
    condition.lock();
    if (_waitingCondition == kAvailable)
    {
      condition.signal();
    }
    condition.unlock();
#endif

    return size;
  }

  int get(Real* outputData, int outputSize)
  {
    int size = _available;
    if (size > outputSize) size = outputSize;

    assert(size <= _bufferSize);
    if (_readIndex + size > _bufferSize)
    {
      int n = _bufferSize - _readIndex;
      memcpy( outputData, &_buffer[_readIndex], n * sizeof(AudioSample));
      memcpy( &outputData[n], _buffer, (size - n)*sizeof(AudioSample));
      _readIndex = (size - n);
    } else {
      memcpy( outputData, &_buffer[_readIndex], size * sizeof(AudioSample));
      _readIndex += size;
    }
    _available -= size;
    _space += size;

    // the thread that is using this ringbuffer will be waiting for
    // space in the buffer - typically the essentia-part from
    // a RingBufferOutput. we signal the waiting condition here
#ifdef CPP_11
    std::lock_guard<std::mutex> lock{ mutex };
    if (_waitingCondition == kSpace)
    {
        condition.notify_one();
    }
#else
    condition.lock();
    if (_waitingCondition == kSpace)
    {
      condition.signal();
    }
    condition.unlock();
#endif

    return size;
  }

};

} // namespace streaming
} // namespace essentia

#endif // ESSENTIA_STREAMING_RINGBUFFERIMPL_H
