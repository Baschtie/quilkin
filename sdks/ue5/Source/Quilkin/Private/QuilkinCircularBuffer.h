/*
 * Copyright 2023 Google LLC
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
#pragma once

#include <deque>
#include <algorithm>

/* A circular first-in last-out buffer used for taking the median of measurements */
template <typename T>
class CircularBuffer
{
public:
    explicit CircularBuffer(size_t InCapacity = 50)
        : Capacity(InCapacity)
    {
    }

    void Add(const T& value)
    {
        if (Buffer.size() == Capacity)
        {
            Buffer.pop_front();
        }
        Buffer.push_back(value);
    }

    bool IsEmpty()
    {
		return Num() == 0;
    }

    size_t Num()
    {
		return Buffer.size();
    }

    T Median() const
    {
        if (Buffer.empty())
        {
            return T{}; // Return default value if the buffer is empty
        }

        std::deque<T> Sorted(Buffer);
        std::sort(Sorted.begin(), Sorted.end());

        size_t Middle = Sorted.size() / 2;

        return (Sorted.size() % 2 == 0) ? (Sorted[Middle] + Sorted[Middle - 1]) / 2 : Sorted[Middle];
    }

private:
    size_t Capacity;
    std::deque<T> Buffer;
};

