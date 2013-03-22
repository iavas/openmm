/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2010-2012 Stanford University and the Authors.      *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation, either version 3 of the License, or       *
 * (at your option) any later version.                                        *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU Lesser General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.      *
 * -------------------------------------------------------------------------- */

#include "CudaSort.h"
#include "CudaKernelSources.h"
#include <map>

using namespace OpenMM;
using namespace std;

CudaSort::CudaSort(CudaContext& context, SortTrait* trait, unsigned int length) : context(context), trait(trait),
        dataRange(NULL), bucketOfElement(NULL), offsetInBucket(NULL), bucketOffset(NULL), buckets(NULL), dataLength(length) {
    // Create kernels.

    map<string, string> replacements;
    replacements["DATA_TYPE"] = trait->getDataType();
    replacements["KEY_TYPE"] =  trait->getKeyType();
    replacements["SORT_KEY"] = trait->getSortKey();
    replacements["MIN_KEY"] = trait->getMinKey();
    replacements["MAX_KEY"] = trait->getMaxKey();
    replacements["MAX_VALUE"] = trait->getMaxValue();
    CUmodule module = context.createModule(context.replaceStrings(CudaKernelSources::sort, replacements));
    shortListKernel = context.getKernel(module, "sortShortList");
    computeRangeKernel = context.getKernel(module, "computeRange");
    assignElementsKernel = context.getKernel(module, "assignElementsToBuckets");
    computeBucketPositionsKernel = context.getKernel(module, "computeBucketPositions");
    copyToBucketsKernel = context.getKernel(module, "copyDataToBuckets");
    sortBucketsKernel = context.getKernel(module, "sortBuckets");

    // Work out the work group sizes for various kernels.

    int maxBlockSize;
    cuDeviceGetAttribute(&maxBlockSize, CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X, context.getDevice());
    int maxSharedMem;
    cuDeviceGetAttribute(&maxSharedMem, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK, context.getDevice());
    unsigned int maxLocalBuffer = (unsigned int) ((maxSharedMem/trait->getDataSize())/2);
    isShortList = (length <= maxLocalBuffer);
    for (rangeKernelSize = 1; rangeKernelSize*2 <= maxBlockSize; rangeKernelSize *= 2)
        ;
    positionsKernelSize = rangeKernelSize;
    sortKernelSize = (isShortList ? rangeKernelSize/2 : rangeKernelSize/4);
    if (rangeKernelSize > length)
        rangeKernelSize = length;
    if (sortKernelSize > maxLocalBuffer)
        sortKernelSize = maxLocalBuffer;
    unsigned int targetBucketSize = sortKernelSize/2;
    unsigned int numBuckets = length/targetBucketSize;
    if (numBuckets < 1)
        numBuckets = 1;
    if (positionsKernelSize > numBuckets)
        positionsKernelSize = numBuckets;

    // Create workspace arrays.

    if (!isShortList) {
        dataRange = new CudaArray(context, 2, trait->getKeySize(), "sortDataRange");
        bucketOffset = CudaArray::create<uint1>(context, numBuckets, "bucketOffset");
        bucketOfElement = CudaArray::create<uint1>(context, length, "bucketOfElement");
        offsetInBucket = CudaArray::create<uint1>(context, length, "offsetInBucket");
        buckets = new CudaArray(context, length, trait->getDataSize(), "buckets");
    }
}

CudaSort::~CudaSort() {
    delete trait;
    if (dataRange != NULL)
        delete dataRange;
    if (bucketOfElement != NULL)
        delete bucketOfElement;
    if (offsetInBucket != NULL)
        delete offsetInBucket;
    if (bucketOffset != NULL)
        delete bucketOffset;
    if (buckets != NULL)
        delete buckets;
}

void CudaSort::sort(CudaArray& data) {
    if (data.getSize() != dataLength || data.getElementSize() != trait->getDataSize())
        throw OpenMMException("CudaSort called with different data size");
    if (data.getSize() == 0)
        return;
    if (isShortList) {
        // We can use a simpler sort kernel that does the entire operation at once in local memory.
        
        void* sortArgs[] = {&data.getDevicePointer(), &dataLength};
        context.executeKernel(shortListKernel, sortArgs, sortKernelSize, sortKernelSize, dataLength*trait->getDataSize());
    }
    else {
        // Compute the range of data values.

        void* rangeArgs[] = {&data.getDevicePointer(), &dataLength, &dataRange->getDevicePointer()};
        context.executeKernel(computeRangeKernel, rangeArgs, rangeKernelSize, rangeKernelSize, rangeKernelSize*trait->getKeySize());

        // Assign array elements to buckets.

        unsigned int numBuckets = bucketOffset->getSize();
        context.clearBuffer(*bucketOffset);
        void* elementsArgs[] = {&data.getDevicePointer(), &dataLength, &numBuckets, &dataRange->getDevicePointer(),
                &bucketOffset->getDevicePointer(), &bucketOfElement->getDevicePointer(), &offsetInBucket->getDevicePointer()};
        context.executeKernel(assignElementsKernel, elementsArgs, data.getSize());

        // Compute the position of each bucket.

        void* computeArgs[] = {&numBuckets, &bucketOffset->getDevicePointer()};
        context.executeKernel(computeBucketPositionsKernel, computeArgs, positionsKernelSize, positionsKernelSize, positionsKernelSize*sizeof(int));

        // Copy the data into the buckets.

        void* copyArgs[] = {&data.getDevicePointer(), &buckets->getDevicePointer(), &dataLength, &bucketOffset->getDevicePointer(),
                &bucketOfElement->getDevicePointer(), &offsetInBucket->getDevicePointer()};
        context.executeKernel(copyToBucketsKernel, copyArgs, data.getSize());

        // Sort each bucket.

        void* sortArgs[] = {&data.getDevicePointer(), &buckets->getDevicePointer(), &numBuckets, &bucketOffset->getDevicePointer()};
        context.executeKernel(sortBucketsKernel, sortArgs, ((data.getSize()+sortKernelSize-1)/sortKernelSize)*sortKernelSize, sortKernelSize, sortKernelSize*trait->getDataSize());
    }
}
