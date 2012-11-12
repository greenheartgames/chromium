// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_ZIP_H_
#define CHROME_COMMON_ZIP_H_

#include "base/callback.h"
#include "base/file_path.h"

namespace zip {

// Zip the contents of src_dir into dest_file. src_path must be a directory.
// An entry will *not* be created in the zip for the root folder -- children
// of src_dir will be at the root level of the created zip. For each file in
// src_dir, include it only if the callback |filter_cb| returns true. Otherwise
// omit it.
typedef base::Callback<bool(const FilePath&)> FilterCallback;
bool ZipWithFilterCallback(const FilePath& src_dir, const FilePath& dest_file,
                           const FilterCallback& filter_cb);

// Convenience method for callers who don't need to set up the filter callback.
// If |include_hidden_files| is true, files starting with "." are included.
// Otherwise they are omitted.
bool Zip(const FilePath& src_dir, const FilePath& dest_file,
         bool include_hidden_files);

// Zips files listed in |src_relative_paths| to |dest_file|.
// The paths listed in |src_relative_paths| are relative to |src_dir| and will
// be used as the file names in the created zip file. All source paths must be
// under |src_dir| in the file system hierarchy.
bool ZipFiles(const FilePath& src_dir,
              const std::vector<FilePath>& src_relative_paths,
              const FilePath& dest_file);

// Unzip the contents of zip_file into dest_dir.
bool Unzip(const FilePath& zip_file, const FilePath& dest_dir);

}  // namespace zip

#endif  // CHROME_COMMON_ZIP_H_
