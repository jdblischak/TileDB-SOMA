/**
 * @file   array_buffers.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2022 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file defines the ArrayBuffers class.
 */

#include "array_buffers.h"
#include "../utils/logger.h"

#include <format>

namespace tiledbsoma {

using namespace tiledb;

std::shared_ptr<ColumnBuffer> ArrayBuffers::at(const std::string& name) {
    if (!contains(name)) {
        throw TileDBSOMAError(
            std::format("[ArrayBuffers] column '{}' does not exist", name));
    }
    return buffers_[name];
}

void ArrayBuffers::emplace(
    const std::string& name, std::shared_ptr<ColumnBuffer> buffer) {
    if (contains(name)) {
        throw TileDBSOMAError(
            std::format("[ArrayBuffers] column '{}' already exists", name));
    }
    names_.push_back(name);
    buffers_.emplace(name, buffer);
}

}  // namespace tiledbsoma