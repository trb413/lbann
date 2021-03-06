////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
// data_reader_imagenet_single .hpp .cpp - data reader class for ImageNet
//                                         dataset packed into a single file
////////////////////////////////////////////////////////////////////////////////

#include "lbann/data_readers/data_reader_imagenet_single.hpp"
#include "lbann/data_readers/image_utils.hpp"
#include <fstream>
#include <omp.h>

namespace lbann {

imagenet_reader_single::imagenet_reader_single(const std::shared_ptr<cv_process>& pp, bool shuffle)
  : imagenet_reader(pp, shuffle) {
}

imagenet_reader_single::imagenet_reader_single(const imagenet_reader_single& source)
  : imagenet_reader(source) {
  m_offsets = source.m_offsets;
  open_data_stream();
}

// Assignment operator
imagenet_reader_single& imagenet_reader_single::operator=(const imagenet_reader_single& source) {
  // check for self-assignment
  if (this == &source) {
    return *this;
  }

  // Call the parent operator= function
  imagenet_reader::operator=(source);

  m_offsets = source.m_offsets;
  open_data_stream();

  return (*this);
}

imagenet_reader_single::~imagenet_reader_single() {
  for(auto & i : m_data_filestream) {
    if (i) delete i;
  }
}

void imagenet_reader_single::load() {
  const std::string image_dir = get_file_dir();
  const std::string base_filename = get_data_filename();

  //open offsets file, with error checking
  std::stringstream b;
  b << image_dir << "/" << base_filename << "_offsets.txt";
  if (is_master()) {
    std::cout << "opening: " << b.str() << " " << std::endl;
  }
  std::ifstream in(b.str().c_str());
  if (not in.is_open() and in.good()) {
    std::stringstream err;
    err << __FILE__ << " " << __LINE__
        << " ::  failed to open " << b.str() << " for reading";
    throw lbann_exception(err.str());
  }

  //read the offsets file
  size_t num_images = 0u;
  in >> num_images;

  if (is_master()) {
    std::cout << "num images: " << num_images << std::endl;
  }

  m_offsets.reserve(num_images+1);
  m_offsets.emplace_back(0,0);
  size_t last_offset = 0u;
  size_t offset = 0u;
  int label = 0;

  while (in >> offset >> label) {
    m_offsets.emplace_back(offset + last_offset, label);
    last_offset = m_offsets.back().first;
  }

  if (num_images+1 != m_offsets.size()) {
    std::stringstream err;
    err << __FILE__ << " " << __LINE__
        << " ::  we read " << m_offsets.size() << " offsets, but should have read " << num_images;
    throw lbann_exception(err.str());
  }
  in.close();

  open_data_stream();

  m_shuffled_indices.resize(num_images);
  for (size_t n = 0; n < num_images; n++) {
    m_shuffled_indices[n] = n;
  }

  select_subset_of_data();
}


bool imagenet_reader_single::fetch_datum(CPUMat& X, int data_id, int mb_idx, int tid) {
  std::stringstream err;

  if (static_cast<size_t>(data_id+1) >= m_offsets.size()) {
    err << __FILE__ << " " << __LINE__ << " :: data_id= " << data_id << " is larger than m_offsets.size()= " << m_offsets.size() << " -2";
    throw lbann_exception(err.str());
  }
  const size_t start = m_offsets[data_id].first;
  const size_t end = m_offsets[data_id+1].first;

  if (end > m_file_size) {
    err << __FILE__ << " " << __LINE__ << " :: end= " << end << " is larger than m_file_size= " << m_file_size << " for P_" << get_rank() << " with role: " << get_role() << " m_offsets.size(): " << m_offsets.size() << " mb_idx: " << mb_idx << " data_id: " << data_id;
    throw lbann_exception(err.str());
  }

  const int ssz = end - start;

  if (ssz <= 0) {
    err << "P_" << get_rank() << " start: " << start << " end: " << end << " ssz= " << ssz << " is <= 0";
    throw lbann_exception(err.str());
  }

  m_work_buffer[tid].resize(ssz);
  m_data_filestream[tid]->seekg(start);
  m_data_filestream[tid]->read((char *)&m_work_buffer[tid][0], ssz);

  int width=0, height=0, img_type=0;
  ::Mat X_v = create_datum_view(X, mb_idx);

  const bool ret = image_utils::import_image(m_work_buffer[tid], width, height, img_type, *(m_pps[tid]), X_v);

  if(!ret) {
    err << __FILE__ << " " << __LINE__ << " :: " << get_type() << ": image_utils::import_image failed to load index: " << data_id;
    throw lbann_exception(err.str());
  }
  if ((width * height * CV_MAT_CN(img_type)) != m_image_linearized_size) {
    err << __FILE__ << " " << __LINE__ << " :: " << get_type() << ": mismatch data size -- either width, height or channel";
    throw lbann_exception(err.str());
  }

  return true;
}

bool imagenet_reader_single::fetch_label(CPUMat& Y, int data_id, int mb_idx, int tid) {
  const int label = m_offsets[data_id+1].second;
  Y.Set(label, mb_idx, 1);
  return true;
}

void imagenet_reader_single::open_data_stream() {
  const std::string image_dir = get_file_dir();
  const std::string base_filename = get_data_filename();

  std::stringstream b;
  b << image_dir << "/" << base_filename << "_data.bin";
  if (is_master()) {
    std::cout << "opening: " << b.str() << " " << std::endl;
  }

  const int nthreads = omp_get_max_threads();
  m_work_buffer.resize(nthreads);

  for(auto & i : m_data_filestream) {
    if (i) delete i;
  }
  m_data_filestream.clear();
  m_data_filestream.resize(nthreads);

  #pragma omp parallel for schedule(static, 1)
  for(int i=0; i < nthreads; ++i) {
    m_data_filestream[i] = new std::ifstream(b.str().c_str(), std::ios::in | std::ios::binary);
    if (m_data_filestream[i] && (not m_data_filestream[i]->is_open() or not m_data_filestream[i]->good())) {
      std::stringstream err;
      err << __FILE__ << " " << __LINE__
          << " ::  failed to open " << b.str() << " for reading";
      throw lbann_exception(err.str());
    }
    m_data_filestream[i]->unsetf(std::ios::skipws);
    if (i==0) {
      m_data_filestream[i]->seekg(0, m_data_filestream[i]->end);
      m_file_size = m_data_filestream[i]->tellg();
    }
    m_data_filestream[i]->seekg(0, m_data_filestream[i]->beg);
  }
}

}  // namespace lbann
