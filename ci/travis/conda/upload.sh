#!/bin/bash

set -e

if [ -z "${ANACONDA_TOKEN+x}" ]
then
    echo "Anaconda token is not set!"
    exit 1
fi

if [ -z "${CONDA_SUBDIR+x}" ]; then
    echo "CONDA_SUBDIR is not set!"
    exit 1
fi

ls
pwd
find .

files=$(find . -name "*gdal*.conda")

if [ -z "$files" ]; then
  echo "No packages matching *gdal*.conda to upload found"
  exit 1
fi

echo "Anaconda token is available, attempting to upload"
conda install -c conda-forge python=3.12 anaconda-client -y

for f in $files; do
  filename=$(basename "$f")

  # extract package name
  pkg=$(echo "$filename" | sed -E 's/-[0-9].*//')

  # extract version number (e.g. 3.12.99)
  version=$(echo "$filename" | sed -E 's/^.+-([0-9]+\.[0-9]+\.[0-9]+).*/\1/')

  echo "Removing gdal-master/$pkg/$version/$CONDA_SUBDIR"
  anaconda -t "$ANACONDA_TOKEN" remove -f "gdal-master/$pkg/$version/$CONDA_SUBDIR" || true

  echo "Uploading $filename"
  anaconda -t "$ANACONDA_TOKEN" upload --force --no-progress --user gdal-master "$f"
done
