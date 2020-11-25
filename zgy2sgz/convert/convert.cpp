/** Copyright 2020 Equinor
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

#include "convert.h"

#include <fstream>
#include <cstring>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "zfp.h"


// Yes, bits_per_voxel can be fractional, but we're statically typed so we'll find out when this is a problem
MODULE_API void convertFile(const char *infile, const char *outfile, int bits_per_voxel)
{
    const std::string infilename(infile);
    const std::string outfilename(outfile);

    // open file and get meta-data from it
    ZgyApi::Error           error;
    ZgyApi::ReaderFactory*  factory(ZgyApi::getReaderFactory());
    ZgyApi::ReaderPtr       accessor;
    MyMetaData              meta;
    ZGYAPI_ASSERT(factory->create(ZgyApi::SimpleString(infilename.c_str()), &accessor, &error));
    ZGYAPI_ASSERT(accessor->getMetaData(&meta, &error));

    int size_pad[3];
    int il_group_size = 64;
    const int pad_dim = 2048 / bits_per_voxel;
    size_pad[0] = 4 * ((meta.size[0] + 3) / 4);
    size_pad[1] = 4 * ((meta.size[1] + 3) / 4);
    size_pad[2] = pad_dim * ((meta.size[2] + pad_dim - 1) / pad_dim);

    // Very large files may have inlines which are too long for Schlumberger's implementation of ZgyReader
    // to read a full 64x64x64 brick-depth of. The limit appears to be 4GB, so suspect they use int where they
    // should be using size_t. Reduce il_group_size correspondingly and hope that the disk buffers save you.
    // Actually the size limit might be 2GB from zfp...
    while (il_group_size*size_pad[1]*size_pad[2]*sizeof(float) >= INT_MAX) {
        il_group_size /= 2;
    }

    std::ofstream outfile_handle (outfilename.c_str(), std::ofstream::binary);

    writeHeader(outfile_handle, meta, size_pad, pad_dim, bits_per_voxel);

    const size_t compressed_size = (il_group_size*size_pad[1]*size_pad[2])*bits_per_voxel / 8;
    void* data = malloc(compressed_size);
    bitstream* bstream = stream_open(data, compressed_size);

    zfp_stream* stream = zfp_stream_open(NULL);
    zfp_stream_set_bit_stream(stream, bstream);
    zfp_stream_set_rate(stream, bits_per_voxel, zfp_type_float, 3, 0);
    zfp_stream_set_execution(stream, zfp_exec_omp);

    const size_t bufsize_pad(il_group_size*size_pad[1]*size_pad[2]);
    Buffer<float> buf(bufsize_pad);
    zfp_field* field = zfp_field_3d(buf.get(), zfp_type_float, size_pad[2], size_pad[1], il_group_size);

    for (int i=0; i<(size_pad[0] +il_group_size -1)/il_group_size; i++) {
        std::memset(buf.get(), 0, bufsize_pad*sizeof(float));
        ZGYAPI_ASSERT(accessor->read(i*il_group_size, 0, 0, il_group_size, size_pad[1], size_pad[2], buf.get(), &error));
        zfp_stream_rewind(stream);
        zfp_compress(stream, field);
        if ((i+1)*il_group_size > size_pad[0]) {
            outfile_handle.write((char*)data, ((size_pad[0]-i*il_group_size)*size_pad[1]*size_pad[2]*bits_per_voxel)/8);
        } else {
            outfile_handle.write((char*)data, compressed_size);
        }
    }
    outfile_handle.flush();
    free(data);

    writeFooter(outfile_handle, meta);
    outfile_handle.close();
}

void writeHeader(std::ofstream& outfile_handle, MyMetaData meta, int size_pad[3], int pad_dim, int bits_per_voxel)
{
    void* header = calloc(4096, 1);
    ((unsigned*)header)[0] = 1;
    ((unsigned*)header)[1] = meta.size[2];
    ((unsigned*)header)[2] = meta.size[1];
    ((unsigned*)header)[3] = meta.size[0];
    ((unsigned*)header)[4] = (unsigned)meta.z0;
    ((unsigned*)header)[5] = (unsigned)meta.hannot0[1];
    ((unsigned*)header)[6] = (unsigned)meta.hannot0[0];
    ((unsigned*)header)[7] = (unsigned)meta.dz;
    ((unsigned*)header)[8] = (unsigned)meta.dhannot[1];
    ((unsigned*)header)[9] = (unsigned)meta.dhannot[0];
    ((int*)header)[10] = bits_per_voxel;
    ((unsigned*)header)[11] = 4;
    ((unsigned*)header)[12] = 4;
    ((unsigned*)header)[13] = pad_dim;
    ((unsigned*)header)[14] = ((((size_pad[0]*size_pad[1]) / 8) * size_pad[2]) / 4096) * bits_per_voxel;
    ((unsigned*)header)[15] = meta.size[0] * meta.size[1] * sizeof(int);
    ((unsigned*)header)[16] = 4;

    // CDP_X CDP_Y
    ((unsigned*)header)[458] = 181;
    ((unsigned*)header)[460] = 181;
    ((unsigned*)header)[461] = 185;
    ((unsigned*)header)[463] = 185;

    // INLINE_3D CROSSLINE_3D
    ((unsigned*)header)[464] = 189;
    ((unsigned*)header)[466] = 189;
    ((unsigned*)header)[467] = 193;
    ((unsigned*)header)[469] = 193;

    outfile_handle.write((char*)header, 4096);
    outfile_handle.flush();
    free(header);
}

void writeFooter(std::ofstream& outfile_handle, MyMetaData meta)
{
    int trace_count = meta.size[0]*meta.size[1];
    int* cdpx_headers = new int[trace_count];
    int* cdpy_headers = new int[trace_count];
    int* il_headers = new int[trace_count];
    int* xl_headers = new int[trace_count];

    double easting_increment_il = (meta.hcornerxy[1][0] - meta.hcornerxy[0][0]) / (meta.size[0] - 1);
    double northing_increment_il = (meta.hcornerxy[1][1] - meta.hcornerxy[0][1]) / (meta.size[0] - 1);

    double easting_increment_xl = (meta.hcornerxy[2][0] - meta.hcornerxy[0][0]) / (meta.size[1] - 1);
    double northing_increment_xl = (meta.hcornerxy[2][1] - meta.hcornerxy[0][1]) / (meta.size[1] - 1);

    for (int il=0; il<meta.size[0]; il++){
        for (int xl=0; xl<meta.size[1]; xl++){
            cdpx_headers[il*meta.size[1] + xl] = (int) round(100.0 * (meta.hcornerxy[0][0] + il*easting_increment_il + xl*easting_increment_xl));
            cdpy_headers[il*meta.size[1] + xl] = (int) round(100.0 * (meta.hcornerxy[0][1] + il*northing_increment_il + xl*northing_increment_xl));
            il_headers[il*meta.size[1] + xl] = (int) (meta.hannot0[0] + il*meta.dhannot[0]);
            xl_headers[il*meta.size[1] + xl] = (int) (meta.hannot0[1] + xl*meta.dhannot[1]);
        }
    }
    outfile_handle.write((char*)cdpx_headers, trace_count*sizeof(int));
    outfile_handle.write((char*)cdpy_headers, trace_count*sizeof(int));
    outfile_handle.write((char*)il_headers, trace_count*sizeof(int));
    outfile_handle.write((char*)xl_headers, trace_count*sizeof(int));
    outfile_handle.flush();

    delete[] cdpx_headers;
    delete[] cdpy_headers;
    delete[] il_headers;
    delete[] xl_headers;
}
