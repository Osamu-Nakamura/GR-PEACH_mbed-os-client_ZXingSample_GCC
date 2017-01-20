/*
 *  Copyright 2010-2011 ZXing authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "ImageReaderSource.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <string>


inline char ImageReaderSource::convertPixel(char const* pixel_) const {
  unsigned char const* pixel = (unsigned char const*)pixel_;
  if (comps == 1 || comps == 2) {
    // Gray or gray+alpha
    return pixel[0];
  } if (comps == 3 || comps == 4) {
    // Red, Green, Blue, (Alpha)
    // We assume 16 bit values here
    // 0x200 = 1<<9, half an lsb of the result to force rounding
    return (char)((306 * (int)pixel[0] + 601 * (int)pixel[1] +
        117 * (int)pixel[2] + 0x200) >> 10);
  } else {
    throw zxing::IllegalArgumentException("Unexpected image depth");
  }
}

ImageReaderSource::ImageReaderSource(ArrayRef<char> image_, int width, int height, int comps_)
    : Super(width, height), image(image_), comps(comps_) {}

Ref<LuminanceSource> ImageReaderSource::create(char* buf, int buf_size, int width, int height) {
    int comps = 4;
    zxing::ArrayRef<char> image;
//    std::vector<unsigned char> out;

//    out.insert(out.end(), &buf[0], &buf[buf_size]);
    image = zxing::ArrayRef<char>(4 * width * height);
    memcpy(&image[0], &buf[0], buf_size);
    if (!image) {
        ostringstream msg;
        msg << "Loading failed.";
        throw zxing::IllegalArgumentException(msg.str().c_str());
    }

    return Ref<LuminanceSource>(new ImageReaderSource(image, width, height, comps));
}

zxing::ArrayRef<char> ImageReaderSource::getRow(int y, zxing::ArrayRef<char> row) const {
  const char* pixelRow = &image[0] + y * getWidth() * 4;
  if (!row) {
    row = zxing::ArrayRef<char>(getWidth());
  }
  for (int x = 0; x < getWidth(); x++) {
    row[x] = convertPixel(pixelRow + (x * 4));
  }
  return row;
}

/** This is a more efficient implementation. */
zxing::ArrayRef<char> ImageReaderSource::getMatrix() const {
  const char* p = &image[0];
  zxing::ArrayRef<char> matrix(getWidth() * getHeight());
  char* m = &matrix[0];
  for (int y = 0; y < getHeight(); y++) {
    for (int x = 0; x < getWidth(); x++) {
      *m = convertPixel(p);
      m++;
      p += 4;
    }
  }
  return matrix;
}

vector<Ref<Result> > decode(Ref<BinaryBitmap> image, DecodeHints hints) {
    Ref<Reader> reader(new MultiFormatReader);
    return vector<Ref<Result> >(1, reader->decode(image, hints));
}

int decode_image(Ref<LuminanceSource> source, bool hybrid, vector<Ref<Result> > * results) {
//    vector<Ref<Result> > results_local;
    string cell_result;
    int res = -1;

    try {
        Ref<Binarizer> binarizer;
        if (hybrid) {
            binarizer = new HybridBinarizer(source);
        } else {
            binarizer = new GlobalHistogramBinarizer(source);
        }
        DecodeHints hints(DecodeHints::DEFAULT_HINT);
        hints.setTryHarder(false);
        Ref<BinaryBitmap> binary(new BinaryBitmap(binarizer));

//        results_local = decode(binary, hints);
        *results = decode(binary, hints);
        res = 0;
    } catch (const ReaderException& e) {
        cell_result = "zxing::ReaderException: " + string(e.what());
        res = -2;
    } catch (const zxing::IllegalArgumentException& e) {
        cell_result = "zxing::IllegalArgumentException: " + string(e.what());
        res = -3;
    } catch (const zxing::Exception& e) {
        cell_result = "zxing::Exception: " + string(e.what());
        res = -4;
    } catch (const std::exception& e) {
        cell_result = "std::exception: " + string(e.what());
        res = -5;
    }

    if (res == 0) {
//        for (size_t i = 0; i < results_local.size(); i++) {
//            cout << results_local[i]->getText()->getText() << endl;
//        }
//        *results = results_local;
    }

    return res;
}

int ex_decode(uint8_t* buf, int buf_size, int width, int height, vector<Ref<Result> > * results) {
    int h_result = 1;
    int g_result = 1;
    int result = 0;
    Ref<LuminanceSource> source;

    try {
        source = ImageReaderSource::create((char*)buf, buf_size, width, height);
    } catch (const zxing::IllegalArgumentException &e) {
        cerr << e.what() << " (ignoring)" << endl;
    }

    h_result = decode_image(source, true, results);
    if (h_result != 0) {
        g_result = decode_image(source, false, results);
        if (g_result != 0) {
            result =  -1;
        }
    }

    return result;
}
