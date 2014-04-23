/***************************************************************************
*   Copyright 2012 - 2013 Advanced Micro Devices, Inc.
*
*   Licensed under the Apache License, Version 2.0 (the "License");
*   you may not use this file except in compliance with the License.
*   You may obtain a copy of the License at
*
*       http://www.apache.org/licenses/LICENSE-2.0
*
*   Unless required by applicable law or agreed to in writing, software
*   distributed under the License is distributed on an "AS IS" BASIS,
*   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*   See the License for the specific language governing permissions and
*   limitations under the License.

***************************************************************************/

#if !defined( BOLT_AMP_REDUCE_BY_KEY_INL )
#define BOLT_AMP_REDUCE_BY_KEY_INL

#define KERNEL02WAVES 4
#define KERNEL1WAVES 4
#define WAVESIZE 64

#define LENGTH_TEST 10

#include <iostream>
#include <fstream>

#include "bolt/amp/iterator/iterator_traits.h"
#ifdef ENABLE_TBB
//TBB Includes
#include "bolt/btbb/reduce_by_key.h"
#endif

namespace bolt
{

namespace amp
{

namespace detail
{
/*!
*   \internal
*   \addtogroup detail
*   \ingroup reduction
*   \{
*/


template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction>
//std::pair<OutputIterator1, OutputIterator2>
unsigned int
gold_reduce_by_key_enqueue( InputIterator1 keys_first,
                            InputIterator1 keys_last,
                            InputIterator2 values_first,
                            OutputIterator1 keys_output,
                            OutputIterator2 values_output,
                            const BinaryPredicate binary_pred,
                            const BinaryFunction binary_op )
{
    typedef typename std::iterator_traits< InputIterator1 >::value_type kType;
    typedef typename std::iterator_traits< InputIterator2 >::value_type vType;
    typedef typename std::iterator_traits< OutputIterator1 >::value_type koType;
    typedef typename std::iterator_traits< OutputIterator2 >::value_type voType;
    static_assert( std::is_convertible< vType, voType >::value,
                   "InputIterator2 and OutputIterator's value types are not convertible." );

   int numElements = static_cast< int >( std::distance( keys_first, keys_last ) );

    // do zeroeth element
    *values_output = *values_first;
    *keys_output = *keys_first;
    unsigned int count = 1;
    // rbk oneth element and beyond

    values_first++;
    for ( InputIterator1 key = (keys_first+1); key != keys_last; key++)
    {
        // load keys
        kType currentKey  = *(key);
        kType previousKey = *(key-1);

        // load value
        voType currentValue = *values_first;
        voType previousValue = *values_output;

        previousValue = *values_output;
        // within segment
        if (binary_pred(currentKey, previousKey))
        {
            voType r = binary_op( previousValue, currentValue);
            *values_output = r;
            *keys_output = currentKey;

        }
        else // new segment
        {
            values_output++;
            keys_output++;
            *values_output = currentValue;
            *keys_output = currentKey;
            count++; //To count the number of elements in the output array
        }
        values_first++;
    }

    //return std::pair(keys_output+1, values_output+1);
    return count;
}
    

//  All calls to reduce_by_key end up here, unless an exception was thrown
//  This is the function that sets up the kernels to compile (once only) and execute
template<
    typename DVInputIterator1,
    typename DVInputIterator2,
    typename DVOutputIterator1,
    typename DVOutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction >
unsigned int
reduce_by_key_enqueue(
    control& ctl,
    const DVInputIterator1& keys_first,
    const DVInputIterator1& keys_last,
    const DVInputIterator2& values_first,
    const DVOutputIterator1& keys_output,
    const DVOutputIterator2& values_output,
    const BinaryPredicate& binary_pred,
    const BinaryFunction& binary_op)
{

    concurrency::accelerator_view av = ctl.getAccelerator().default_view;

    /**********************************************************************************
     * Type Names - used in KernelTemplateSpecializer
     *********************************************************************************/
    typedef typename std::iterator_traits< DVInputIterator1 >::value_type kType;
    typedef typename std::iterator_traits< DVInputIterator2 >::value_type vType;
    typedef typename std::iterator_traits< DVOutputIterator1 >::value_type koType;
    typedef typename std::iterator_traits< DVOutputIterator2 >::value_type voType;
   
    int numElements = static_cast< int >( std::distance( keys_first, keys_last ) );
    const unsigned int kernel0_WgSize = WAVESIZE*KERNEL02WAVES;
    const unsigned int kernel1_WgSize = WAVESIZE*KERNEL1WAVES ;
    const unsigned int kernel2_WgSize = WAVESIZE*KERNEL02WAVES;

    //  Ceiling function to bump the size of input to the next whole wavefront size
    unsigned int sizeInputBuff = numElements;
    unsigned int modWgSize = (sizeInputBuff & (kernel0_WgSize-1));
    if( modWgSize )
    {
        sizeInputBuff &= ~modWgSize;
        sizeInputBuff += kernel0_WgSize;
    }
    int numWorkGroupsK0 = static_cast< int >( sizeInputBuff / (kernel0_WgSize) );
    //  Ceiling function to bump the size of the sum array to the next whole wavefront size
    unsigned int sizeScanBuff = numWorkGroupsK0;
    modWgSize = (sizeScanBuff & (kernel0_WgSize-1));
    if( modWgSize )
    {
        sizeScanBuff &= ~modWgSize;
        sizeScanBuff += kernel0_WgSize;
    }

    concurrency::array< int >  keySumArray( sizeScanBuff, av );
    concurrency::array< voType >  preSumArray( sizeScanBuff, av );
    concurrency::array< voType >  postSumArray( sizeScanBuff, av );
    
	concurrency::array< int >  tempArray( numElements, av );
    device_vector<int> newKeyArray(tempArray);

    /**********************************************************************************
     *  Kernel 0
     *********************************************************************************/
    
    concurrency::extent< 1 > globalSizeK0( sizeInputBuff );
	try
	{
		concurrency::parallel_for_each( av, globalSizeK0,
			[
				keys_first,
				binary_pred,
				numElements,
				newKeyArray,	
				kernel0_WgSize
			] ( concurrency::index< 1 > t_idx ) restrict(amp)
	   {

			int gloId = t_idx[ 0 ];


			if (gloId >= numElements)
				return;

			kType key, prev_key;

			if(gloId > 0){
			  key = keys_first[ gloId ];
			  prev_key = keys_first[ gloId - 1];
			  if(binary_pred(key, prev_key))
				newKeyArray[ gloId ] = 0;
			  else
				newKeyArray[ gloId ] = 1;
			}
			else{
				newKeyArray[ gloId ] = 0;
			}
    
	   } );
	}
	catch(std::exception &e)
    {
        std::cout << "Exception while calling bolt::amp::reduce_by_key parallel_for_each " ;
        std::cout<< e.what() << std::endl;
        throw std::exception();
    }	
    detail::scan_enqueue(ctl, newKeyArray.begin(), newKeyArray.end(), newKeyArray.begin(), 0, bolt::amp::plus< int >( ), true);

    /**********************************************************************************
     *  Kernel 1
     *********************************************************************************/
   //	This loop is inherently parallel; every tile is independant with potentially many wavefronts
	try
	{

		#define MSVC_TILE_LIMIT 65535

			unsigned int max_extent = MSVC_TILE_LIMIT * kernel0_WgSize;
			unsigned int iteration = sizeInputBuff/max_extent;
			/* Check if second iteration is required */
			if (sizeInputBuff % max_extent )
				iteration++;

			max_extent = sizeInputBuff > max_extent ? max_extent : sizeInputBuff;

			concurrency::extent< 1 > globalSizeK1( max_extent);
			concurrency::tiled_extent< kernel0_WgSize > tileK1 = globalSizeK1.tile< kernel0_WgSize >();

			for(unsigned int i=0; i < iteration; i++)
			{
	   
				unsigned int global_offset = i  * max_extent;
				unsigned int wg_offset = i* (MSVC_TILE_LIMIT);
				concurrency::parallel_for_each( av, tileK1,
					[
						newKeyArray,
						values_first,
						numElements,
						&keySumArray,
						&preSumArray,
						binary_op,
						kernel0_WgSize,
						global_offset,
						wg_offset
					] ( concurrency::tiled_index< kernel0_WgSize > t_idx )  restrict(amp)
			  {

				int gloId = t_idx.global[ 0 ] + global_offset ;
				int groId = t_idx.tile[ 0 ] + wg_offset ;
				unsigned int locId = t_idx.local[ 0 ];
				unsigned int wgSize = kernel0_WgSize;

				tile_static vType ldsVals[ WAVESIZE*KERNEL02WAVES ];
				tile_static int ldsKeys[ WAVESIZE*KERNEL02WAVES ];

				int key; 
				voType val; 

				if(gloId < numElements){
				  key = newKeyArray[ gloId ];
				  val = values_first[ gloId ];
				  ldsKeys[ locId ] =  key;
				  ldsVals[ locId ] = val;
				}
				// Computes a scan within a workgroup
				// updates vals in lds but not keys
				voType sum = val; 
				for( unsigned int offset = 1; offset < wgSize; offset *= 2 )
				{
					t_idx.barrier.wait();
					if (locId >= offset )
					{
						int key2 = ldsKeys[ locId - offset];
						if( key == key2 )
						{
							voType y = ldsVals[ locId - offset];
							sum = binary_op( sum, y );
						}
						else
							sum = ldsVals[ locId];
					
					}
					t_idx.barrier.wait();
					ldsVals[ locId ] = sum;
				}
        		 t_idx.barrier.wait(); // needed for large data types
				//  Abort threads that are passed the end of the input vector
				if (gloId >= numElements)
					return;
				if (locId == 0)
				{
					keySumArray[ groId ] = ldsKeys[ wgSize-1 ];
					preSumArray[ groId ] = ldsVals[ wgSize-1 ];
				}

			  } );

		 }
	 }
	 catch(std::exception &e)
      {
        std::cout << "Exception while calling bolt::amp::reduce_by_key parallel_for_each " ;
        std::cout<< e.what() << std::endl;
        throw std::exception();
      }	

    /**********************************************************************************
     *  Kernel 2
     *********************************************************************************/
    int workPerThread = static_cast< int >( sizeScanBuff / kernel1_WgSize );
    workPerThread = workPerThread ? workPerThread : 1;		
    concurrency::extent< 1 > globalSizeK2( kernel1_WgSize );
    concurrency::tiled_extent< kernel1_WgSize > tileK2 = globalSizeK2.tile< kernel1_WgSize >();
    //std::cout << "Kernel 2 Launching w/ " << sizeInputBuff << " threads for " << numElements << " elements. " << std::endl;

	try
	{
    concurrency::parallel_for_each( av, tileK2,
        [
            &keySumArray,
            &postSumArray,
            &preSumArray,
            numElements,
            binary_op,
            kernel1_WgSize,
            workPerThread
        ] ( concurrency::tiled_index< kernel1_WgSize > t_idx ) restrict(amp)
  {

     int gloId = t_idx.global[ 0 ];
    int groId = t_idx.tile[ 0 ];
    int locId = t_idx.local[ 0 ];
    int wgSize = kernel1_WgSize;
    int mapId  = gloId * workPerThread;

    tile_static vType ldsVals[ WAVESIZE*KERNEL02WAVES ];
    tile_static int ldsKeys[ WAVESIZE*KERNEL02WAVES ];
    

    // do offset of zero manually
    int offset;
    int key;
    voType workSum;

    if (mapId < numElements)
    {
        int prevKey;

        // accumulate zeroth value manually
        offset = 0;
        key = keySumArray[ mapId+offset ];
        workSum = preSumArray[ mapId+offset ];
        postSumArray[ mapId+offset ] = workSum;

        //  Serial accumulation
        for( offset = offset+1; offset < workPerThread; offset += 1 )
        {
            prevKey = key;
            key =  keySumArray[ mapId+offset ];
            if (mapId+offset<numElements )
            {
                voType y = preSumArray[ mapId+offset ];
                if ( key == prevKey )
                {
                    workSum = binary_op( workSum, y );
                }
                else
                {
                    workSum = y;
                }
                postSumArray[ mapId+offset ] = workSum;
            }
        }
    }
    t_idx.barrier.wait();

    voType scanSum = workSum;
    offset = 1;
    // load LDS with register sums
    ldsVals[ locId ] = workSum;
    ldsKeys[ locId ] = key;
    // scan in lds
    for( offset = offset*1; offset < wgSize; offset *= 2 )
    {
        t_idx.barrier.wait();
        if (mapId < numElements)
        {
            if (locId >= offset  )
            {
                voType y = ldsVals[ locId - offset ];
                int key1 = ldsKeys[ locId ];
                int key2 = ldsKeys[ locId-offset ];
                if ( key1 == key2 )
                {
                   scanSum = binary_op( scanSum, y );
                }
                else
                   scanSum = ldsVals[ locId ];
             }

        }
        t_idx.barrier.wait();
        ldsVals[ locId ] = scanSum;
    } // for offset
    t_idx.barrier.wait();

    // write final scan from pre-scan and lds scan
    for( offset = 0; offset < workPerThread; offset += 1 )
    {
        t_idx.barrier.wait();

        if (mapId < numElements && locId > 0)
        {
            voType y = postSumArray[ mapId+offset ];
            int key1 =  keySumArray[ mapId+offset ]; 
            int key2 =  ldsKeys[ locId-1 ];
            if ( key1 == key2 )
            {
                voType y2 = ldsVals[locId-1];
                y = binary_op( y, y2 );
            }
            postSumArray[ mapId+offset ] = y;
        } // thread in bounds
    } // for
  } );
   }

    catch(std::exception &e)
    {
        std::cout << "Exception while calling bolt::amp::reduce_by_key parallel_for_each " ;
        std::cout<< e.what() << std::endl;
        throw std::exception();
    }	

    /**********************************************************************************
     *  Kernel 3
     *********************************************************************************/
    //std::cout << "Kernel 3 Launching w/ " << sizeInputBuff << " threads for " << numElements << " elements. " << std::endl;
	try
	{


    unsigned int max_extent = MSVC_TILE_LIMIT * kernel2_WgSize;
    unsigned int iteration = sizeInputBuff/max_extent;
    /* Check if second iteration is required */
    if (sizeInputBuff % max_extent )
        iteration++;

    max_extent = sizeInputBuff > max_extent ? max_extent : sizeInputBuff;

    concurrency::extent< 1 > globalSizeK3( max_extent );
    concurrency::tiled_extent< kernel2_WgSize > tileK3 = globalSizeK3.tile< kernel2_WgSize >();


    for(unsigned int i=0; i < iteration; i++)
	{

    unsigned int global_offset = i  * max_extent;
    unsigned int wg_offset = i* (MSVC_TILE_LIMIT);
    concurrency::parallel_for_each( av, tileK3,
        [
			values_first,
			keys_first,
			keys_output,
			values_output,
            newKeyArray,
            &keySumArray,
            &postSumArray,
            binary_pred,
            numElements,
            binary_op,
            kernel2_WgSize,
            global_offset,
            wg_offset
        ] ( concurrency::tiled_index< kernel2_WgSize > t_idx ) restrict(amp)
  {

	int gloId = t_idx.global[ 0 ] + global_offset ;
	unsigned int groId = t_idx.tile[ 0 ] + wg_offset ;
	unsigned int locId = t_idx.local[ 0 ];
	unsigned int wgSize = kernel2_WgSize;
	
	tile_static vType ldsVals[ WAVESIZE*KERNEL02WAVES ];
	tile_static int ldsKeys[ WAVESIZE*KERNEL02WAVES ];
	
	int key; 
	voType val; 
	if(gloId < numElements)
	{
		  key = newKeyArray[ gloId ];
		  val = values_first[ gloId ];
		  ldsKeys[ locId ] =  key;
		  ldsVals[ locId ] = val;
	}
	// Computes a scan within a workgroup
	// updates vals in lds but not keys
	voType sum = val; 
	for( unsigned int offset = 1; offset < wgSize; offset *= 2 )
	{
		t_idx.barrier.wait();
		if (locId >= offset )
		{
			int key2 = ldsKeys[ locId - offset];
			if( key == key2 )
			{
				voType y = ldsVals[ locId - offset];
				sum = binary_op( sum, y );
			}
			else
				sum = ldsVals[ locId];
		}
		t_idx.barrier.wait();
		ldsVals[ locId ] = sum;
	}
    t_idx.barrier.wait(); // needed for large data types

    //  Abort threads that are passed the end of the input vector
    if( gloId >= numElements )
        return;

    // accumulate prefix
    int  key1 =  keySumArray[ groId-1 ];
    int  key2 =  newKeyArray[ gloId ];
    int  key3 = -1;
    if(gloId < numElements -1 )
      key3 =  newKeyArray[ gloId + 1];
    if (groId > 0 && key1 == key2 && key2 != key3)
    {
        voType scanResult = sum;
        voType postBlockSum = postSumArray[ groId-1 ];
        voType newResult = binary_op( scanResult, postBlockSum );
        sum = newResult;

    }
	unsigned int count_number_of_sections = 0;		
    count_number_of_sections = newKeyArray[numElements-1] + 1;
    if(gloId < (numElements-1) && newKeyArray[ gloId ] != newKeyArray[ gloId +1])
    {
        keys_output [newKeyArray [ gloId ]] = keys_first[ gloId];
        values_output[ newKeyArray [ gloId ]] = sum;
    }

    if( gloId == (numElements-1) )
    {
        keys_output[ count_number_of_sections - 1 ] = keys_first[ gloId ]; //Copying the last key directly. Works either ways
        values_output[ count_number_of_sections - 1 ] = sum;
        newKeyArray [ gloId ] = count_number_of_sections;
    }

  } );

    }
	}
	 catch(std::exception &e)
      {
        std::cout << "Exception while calling bolt::amp::reduce_by_key parallel_for_each " ;
        std::cout<< e.what() << std::endl;
        throw std::exception();
      }	
	unsigned int count_number_of_sections = 0;
    count_number_of_sections = newKeyArray[numElements-1];
    return count_number_of_sections;

}   //end of reduce_by_key_enqueue( )



/*!
* \brief This overload is called strictly for non-device_vector iterators
* \details This template function overload is used to seperate device_vector iterators from all other iterators
*/
template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction >
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key_pick_iterator(
    control& ctl,
    const InputIterator1& keys_first,
    const InputIterator1& keys_last,
    const InputIterator2& values_first,
    const OutputIterator1& keys_output,
    const OutputIterator2& values_output,
    const BinaryPredicate& binary_pred,
    const BinaryFunction& binary_op,
	std::random_access_iterator_tag,
	std::random_access_iterator_tag )
{

    typedef typename std::iterator_traits< InputIterator1 >::value_type kType;
    typedef typename std::iterator_traits< InputIterator2 >::value_type vType;
    typedef typename std::iterator_traits< OutputIterator1 >::value_type koType;
    typedef typename std::iterator_traits< OutputIterator2 >::value_type voType;
    static_assert( std::is_convertible< vType, voType >::value, "InputValue and Output iterators are incompatible" );

    int numElements = static_cast< int >( std::distance( keys_first, keys_last ) );
    if( numElements == 1 )
        return  bolt::amp::make_pair( keys_last, values_first+numElements );

    bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
    if(runMode == bolt::amp::control::Automatic) {
        runMode = ctl.getDefaultPathToRun();
    }
   
    if (runMode == bolt::amp::control::SerialCpu) {
          
            unsigned int sizeOfOut = gold_reduce_by_key_enqueue( keys_first, keys_last, values_first, keys_output,
            values_output, binary_pred, binary_op);

            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);

    } else if (runMode == bolt::amp::control::MultiCoreCpu) {

        #ifdef ENABLE_TBB     
            unsigned int sizeOfOut = bolt::btbb::reduce_by_key( keys_first, keys_last, values_first, keys_output, values_output, binary_pred, binary_op);
            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
        #else
            throw std::runtime_error("MultiCoreCPU Version of ReduceByKey not Enabled! \n");
        #endif
    }
    else {
    unsigned int sizeOfOut;
    {
        // Map the input iterator to a device_vector
        device_vector< kType, concurrency::array_view > dvKeys(  keys_first, keys_last, false, ctl );
        device_vector< vType, concurrency::array_view > dvValues( values_first, numElements, false, ctl );
        device_vector< koType, concurrency::array_view > dvKOutput( keys_output, numElements, true, ctl );
        device_vector< voType, concurrency::array_view > dvVOutput( values_output, numElements, true, ctl );

        //Now call the actual AMP algorithm
        sizeOfOut = reduce_by_key_enqueue( ctl, dvKeys.begin( ), dvKeys.end( ), dvValues.begin(), dvKOutput.begin( ),
                                          dvVOutput.begin( ), binary_pred, binary_op);

        // This should immediately map/unmap the buffer
        dvKOutput.data( );
        dvVOutput.data( );
    }
    return  bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
    }

}

/*!
* \brief This overload is called strictly for device_vector iterators
* \details This template function overload is used to seperate device_vector iterators from all other iterators
*/

    template<
    typename DVInputIterator1,
    typename DVInputIterator2,
    typename DVOutputIterator1,
    typename DVOutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction >
bolt::amp::pair<DVOutputIterator1, DVOutputIterator2>
reduce_by_key_pick_iterator(
    control& ctl,
    const DVInputIterator1& keys_first,
    const DVInputIterator1& keys_last,
    const DVInputIterator2& values_first,
    const DVOutputIterator1& keys_output,
    const DVOutputIterator2& values_output,
    const BinaryPredicate& binary_pred,
    const BinaryFunction& binary_op,
	bolt::amp::device_vector_tag,
	bolt::amp::device_vector_tag )
{

    typedef typename std::iterator_traits< DVInputIterator1 >::value_type kType;
    typedef typename std::iterator_traits< DVInputIterator2 >::value_type vType;
    typedef typename std::iterator_traits< DVOutputIterator1 >::value_type koType;
    typedef typename std::iterator_traits< DVOutputIterator2 >::value_type voType;
    static_assert( std::is_convertible< vType, voType >::value, "InputValue and Output iterators are incompatible" );

    int numElements = static_cast< int >( std::distance( keys_first, keys_last ) );
    if( numElements == 1 )
        return  bolt::amp::make_pair( keys_last, values_first+numElements );

    bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode( );  // could be dynamic choice some day.
    if(runMode == bolt::amp::control::Automatic)
    {
        runMode = ctl.getDefaultPathToRun();
    }
    
    if( runMode == bolt::amp::control::SerialCpu )
    {
     
        typename bolt::amp::device_vector< kType >::pointer keysPtr =  keys_first.getContainer( ).data( );
        typename bolt::amp::device_vector< vType >::pointer valsPtr =  values_first.getContainer( ).data( );
        typename bolt::amp::device_vector< koType >::pointer oKeysPtr =  keys_output.getContainer( ).data( );
        typename bolt::amp::device_vector< voType >::pointer oValsPtr =  values_output.getContainer( ).data( );
        unsigned int sizeOfOut = gold_reduce_by_key_enqueue( &keysPtr[keys_first.m_Index], &keysPtr[keys_first.m_Index + numElements],
                                           &valsPtr[values_first.m_Index], &oKeysPtr[keys_output.m_Index],
                                          &oValsPtr[values_output.m_Index], binary_pred, binary_op);
        return  bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);

    }
    else if( runMode == bolt::amp::control::MultiCoreCpu )
    {
        #ifdef ENABLE_TBB
        typename bolt::amp::device_vector< kType >::pointer keysPtr =  keys_first.getContainer( ).data( );
        typename bolt::amp::device_vector< vType >::pointer valsPtr =  values_first.getContainer( ).data( );
        typename bolt::amp::device_vector< koType >::pointer oKeysPtr =  keys_output.getContainer( ).data( );
        typename bolt::amp::device_vector< voType >::pointer oValsPtr =  values_output.getContainer( ).data( );

        unsigned int sizeOfOut = bolt::btbb::reduce_by_key( &keysPtr[keys_first.m_Index], &keysPtr[keys_first.m_Index + numElements],
                                           &valsPtr[values_first.m_Index], &oKeysPtr[keys_output.m_Index],
                                          &oValsPtr[values_output.m_Index], binary_pred, binary_op);
        return  bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
        #else
            throw std::runtime_error("MultiCoreCPU Version of ReduceByKey not Enabled! \n");
        #endif
    }
    else
    {
            //Now call the actual AMP algorithm
            unsigned int sizeOfOut = reduce_by_key_enqueue( ctl, keys_first, keys_last, values_first, keys_output,
            values_output, binary_pred, binary_op);

            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
    }

}

/*!
* \brief This overload is called strictly for non-device_vector iterators
* \details This template function overload is used to seperate device_vector iterators from all other iterators
*/
template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction >
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key_pick_iterator(
    control& ctl,
    const InputIterator1& keys_first,
    const InputIterator1& keys_last,
    const InputIterator2& values_first,
    const OutputIterator1& keys_output,
    const OutputIterator2& values_output,
    const BinaryPredicate& binary_pred,
    const BinaryFunction& binary_op,
	bolt::amp::fancy_iterator_tag,
	std::random_access_iterator_tag )
{

    typedef typename std::iterator_traits< InputIterator1 >::value_type kType;
    typedef typename std::iterator_traits< InputIterator2 >::value_type vType;
    typedef typename std::iterator_traits< OutputIterator1 >::value_type koType;
    typedef typename std::iterator_traits< OutputIterator2 >::value_type voType;

    int numElements = static_cast< int >( std::distance( keys_first, keys_last ) );
    if( numElements == 1 )
        return  bolt::amp::make_pair( keys_last, values_first+numElements );

    bolt::amp::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
    if(runMode == bolt::amp::control::Automatic) {
        runMode = ctl.getDefaultPathToRun();
    }
   
    if (runMode == bolt::amp::control::SerialCpu) {
          
            unsigned int sizeOfOut = gold_reduce_by_key_enqueue( keys_first, keys_last, values_first, keys_output,
            values_output, binary_pred, binary_op);

            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);

    } else if (runMode == bolt::amp::control::MultiCoreCpu) {

        #ifdef ENABLE_TBB     
            unsigned int sizeOfOut = bolt::btbb::reduce_by_key( keys_first, keys_last, values_first, keys_output, values_output, binary_pred, binary_op);
            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
        #else
            throw std::runtime_error("MultiCoreCPU Version of ReduceByKey not Enabled! \n");
        #endif
    }
    else {
    //Now call the actual AMP algorithm
            unsigned int sizeOfOut = reduce_by_key_enqueue( ctl, keys_first, keys_last, values_first, keys_output,
            values_output, binary_pred, binary_op);
            return   bolt::amp::make_pair(keys_output+sizeOfOut, values_output+sizeOfOut);
    }

}



/*********************************************************************************************************************
 * Detect Random Access
 ********************************************************************************************************************/

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction
    >
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key_detect_random_access(
    control &ctl,
    const InputIterator1  keys_first,
    const InputIterator1  keys_last,
    const InputIterator2  values_first,
    const OutputIterator1  keys_output,
    const OutputIterator2  values_output,
    const BinaryPredicate binary_pred,
    const BinaryFunction binary_op,
    std::random_access_iterator_tag,
	std::random_access_iterator_tag )
{
    return detail::reduce_by_key_pick_iterator( ctl, keys_first, keys_last, values_first, keys_output, values_output,
        binary_pred, binary_op,
		typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( ) );
}

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key_detect_random_access(
    control &ctl,
    const InputIterator1  keys_first,
    const InputIterator1  keys_last,
    const InputIterator2  values_first,
    const OutputIterator1  keys_output,
    const OutputIterator2  values_output,
    const BinaryPredicate binary_pred,
    const BinaryFunction binary_op,
    std::random_access_iterator_tag, bolt::amp::fancy_iterator_tag )
{
    //  TODO:  It should be possible to support non-random_access_iterator_tag iterators, if we copied the data
    //  to a temporary buffer.  Should we?
	static_assert( std::is_same< OutputIterator2, bolt::amp::fancy_iterator_tag >::value , "It is not possible to output to fancy iterators; they are not mutable" );
};

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key_detect_random_access(
    control &ctl,
    const InputIterator1  keys_first,
    const InputIterator1  keys_last,
    const InputIterator2  values_first,
    const OutputIterator1  keys_output,
    const OutputIterator2  values_output,
    const BinaryPredicate binary_pred,
    const BinaryFunction binary_op,
    std::input_iterator_tag, std::input_iterator_tag  )
{
    //  TODO:  It should be possible to support non-random_access_iterator_tag iterators, if we copied the data
    //  to a temporary buffer.  Should we?
    static_assert( std::is_same< InputIterator1, std::input_iterator_tag >::value , "Bolt only supports random access iterator types" );
};

    /*!   \}  */
} //namespace detail



/**********************************************************************************************************************
 * REDUCE BY KEY
 *********************************************************************************************************************/
template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output,
    BinaryPredicate binary_pred,
    BinaryFunction binary_op )
{
    control& ctl = control::getDefault();
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output,
    BinaryPredicate binary_pred)
{
    typedef typename std::iterator_traits<OutputIterator2>::value_type ValOType;
    control& ctl = control::getDefault();
    plus<ValOType> binary_op;
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output)
{
    typedef typename std::iterator_traits<InputIterator1>::value_type kType;
    typedef typename std::iterator_traits<OutputIterator2>::value_type ValOType;
    control& ctl = control::getDefault();
    equal_to <kType> binary_pred;
    plus <ValOType> binary_op;
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}


///////////////////////////// CTRL ////////////////////////////////////////////

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate,
    typename BinaryFunction>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    control &ctl,
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output,
    BinaryPredicate binary_pred,
    BinaryFunction binary_op )
{
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2,
    typename BinaryPredicate>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    control &ctl,
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output,
    BinaryPredicate binary_pred)
{
    typedef typename std::iterator_traits<OutputIterator2>::value_type ValOType;
 
    plus<ValOType> binary_op;
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}

template<
    typename InputIterator1,
    typename InputIterator2,
    typename OutputIterator1,
    typename OutputIterator2>
bolt::amp::pair<OutputIterator1, OutputIterator2>
reduce_by_key(
    control &ctl,
    InputIterator1  keys_first,
    InputIterator1  keys_last,
    InputIterator2  values_first,
    OutputIterator1  keys_output,
    OutputIterator2  values_output)
{
    typedef typename std::iterator_traits<InputIterator1>::value_type kType;
    typedef typename std::iterator_traits<OutputIterator2>::value_type ValOType;

    equal_to <kType> binary_pred;
    plus<ValOType> binary_op;
    return detail::reduce_by_key_detect_random_access(
        ctl,
        keys_first,
        keys_last,
        values_first,
        keys_output,
        values_output,
        binary_pred,
        binary_op,
        typename std::iterator_traits< InputIterator1 >::iterator_category( ),
		typename std::iterator_traits< OutputIterator2 >::iterator_category( )
    ); // return
}

} //namespace amp
} //namespace bolt

#endif


