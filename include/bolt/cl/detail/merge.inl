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


#if !defined( BOLT_CL_MERGE_INL )
#define BOLT_CL_MERGE_INL
#pragma once

#include <algorithm>

#include <boost/thread/once.hpp>
#include <boost/bind.hpp>
#include "bolt/cl/bolt.h"
#include "bolt/cl/functional.h"
#ifdef ENABLE_TBB
//TBB Includes
#include "bolt/btbb/merge.h"
#endif


namespace bolt {
    namespace cl {

        namespace detail {

            ///////////////

        enum MergeTypes {merge_iVType1,merge_iVType2, merge_iIterType1,merge_iIterType2, merge_rIterType,merge_resType,merge_StrictWeakCompare, merge_end};

        ///////////////////////////////////////////////////////////////////////
        //Kernel Template Specializer
        ///////////////////////////////////////////////////////////////////////
        class Merge_KernelTemplateSpecializer : public KernelTemplateSpecializer
            {
            public:

            Merge_KernelTemplateSpecializer() : KernelTemplateSpecializer()
                {
                    addKernelName( "mergeTemplate" );
                }

            const ::std::string operator() ( const ::std::vector<::std::string>& typeNames ) const
            {
                const std::string templateSpecializationString =
                        "// Host generates this instantiation string with user-specified value type and functor\n"
                        "template __attribute__((mangled_name(" + name(0) + "Instantiated)))\n"
                        "__attribute__((reqd_work_group_size(64,1,1)))\n"
                        "__kernel void mergeTemplate(\n"
                        "global " + typeNames[merge_iVType1] + "* input_ptr1,\n"
                         + typeNames[merge_iIterType1] + " iter1,\n"
                        "const int length1,\n"
                        "global " + typeNames[merge_iVType1] + "* input_ptr2,\n"
                         + typeNames[merge_iIterType2] + " iter2,\n"
                       "const int length2,\n"
                        "global " + typeNames[merge_resType] + "* result,\n"
                         + typeNames[merge_rIterType] + " riter,\n"
                        "global " + typeNames[merge_StrictWeakCompare] + "* userFunctor\n"
                        ");\n\n";

                return templateSpecializationString;
            }
            };




            template<typename DVInputIterator1,typename DVInputIterator2,typename DVoutputIterator, 
            typename StrictWeakCompare>
            DVoutputIterator merge_detect_random_access(bolt::cl::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                 DVoutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                std::random_access_iterator_tag)
            {
                return merge_pick_iterator( ctl, first1, last1, first2, last2,result, comp, cl_code,
                    std::iterator_traits< DVInputIterator1 >::iterator_category( ) );
            }

            template<typename DVInputIterator1,typename DVInputIterator2,typename DVoutputIterator, 
            typename StrictWeakCompare>
            DVoutputIterator merge_detect_random_access(bolt::cl::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                 DVoutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                std::input_iterator_tag)
            {
              //  TODO: It should be possible to support non-random_access_iterator_tag iterators,if we copied the data
              //  to a temporary buffer.  Should we?
                static_assert( std::is_same< DVInputIterator, bolt::cl::input_iterator_tag  >::value,
                    "Bolt only supports random access iterator types" );
            }


            // This template is called after we detect random access iterators
            // This is called strictly for any non-device_vector iterator
            template<typename InputIterator1,typename InputIterator2,typename outputIterator, 
            typename StrictWeakCompare>
            outputIterator merge_pick_iterator(bolt::cl::control &ctl,
                const InputIterator1& first1,
                const InputIterator1& last1,
                const InputIterator2& first2,
                const InputIterator2& last2,
                 outputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                std::random_access_iterator_tag )
            {
                /*************/
                typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
                typedef typename std::iterator_traits<InputIterator2>::value_type iType2;
                typedef typename std::iterator_traits<outputIterator>::value_type oType;


                /*TODO - probably the forceRunMode should be replaced by getRunMode and setRunMode*/
                bolt::cl::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.

                if(runMode == bolt::cl::control::Automatic)
                {
                    runMode = ctl.getDefaultPathToRun();
                }

                switch(runMode)
                {
                case bolt::cl::control::OpenCL :
                    {
                        device_vector< iType1 > dvInput1( first1, last1, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, ctl );
                        device_vector< iType2 > dvInput2( first2, last2, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, ctl );
                        device_vector< oType >  dvresult( result, result + (last1-first1) + (last2-first2), CL_MEM_USE_HOST_PTR, ctl );

                        merge_enqueue( ctl, dvInput1.begin(), dvInput1.end(), dvInput2.begin(), dvInput2.end(),
                            dvresult.begin(), comp, cl_code);

                        // This should immediately map/unmap the buffer
                        dvresult.data( );
                        return result + (last1 - first1) + (last2 - first2);
                    }
              
                case bolt::cl::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
                        return bolt::btbb::merge(first1,last1,first2,last2,result,comp);
                    #else
                        throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    #endif

                case bolt::cl::control::SerialCpu: 
                    return std::merge(first1,last1,first2,last2,result,comp);

                default:
                   return std::merge(first1,last1,first2,last2,result,comp);

                }

            }

            // This template is called after we detect random access iterators
            // This is called strictly for iterators that are derived from device_vector< T >::iterator
            template<typename DVInputIterator1,typename DVInputIterator2,typename DVoutputIterator, 
            typename StrictWeakCompare>
            DVoutputIterator merge_pick_iterator(bolt::cl::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                 DVoutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                bolt::cl::device_vector_tag )
            {
                typedef typename std::iterator_traits<DVInputIterator1>::value_type iType1;
                typedef typename std::iterator_traits<DVInputIterator2>::value_type iType2;
                typedef typename std::iterator_traits<DVoutputIterator>::value_type oType;

                size_t szElements = static_cast<size_t>(std::distance(first1, last1) );
                if (szElements == 0)
                    return result;

                bolt::cl::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
                if(runMode == bolt::cl::control::Automatic)
                {
                    runMode = ctl.getDefaultPathToRun();
                }

                switch(runMode)
                {
                case bolt::cl::control::OpenCL :
                        return merge_enqueue( ctl, first1, last1,first2, last2, result, comp, cl_code);
              
                case bolt::cl::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
                    {
                      typename bolt::cl::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename bolt::cl::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename bolt::cl::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                       bolt::btbb::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                               &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                               &mergeResBuffer[result.m_Index],comp);

                         return result + (last1 - first1) + (last2 - first2);
                    }
                    #else
                    {
                        throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    }
                    #endif

                case bolt::cl::control::SerialCpu: 
                    {
                      typename bolt::cl::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename bolt::cl::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename  bolt::cl::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                      std::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                               &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                              &mergeResBuffer[result.m_Index],comp);
                        return result + (last1 - first1) + (last2 - first2);
                    }

                default: /* Incase of runMode not set/corrupted */
                    {
                      typename  bolt::cl::device_vector< iType1 >::pointer mergeInputBuffer1 =  first1.getContainer( ).data( );
                      typename  bolt::cl::device_vector< iType2 >::pointer mergeInputBuffer2 =  first2.getContainer( ).data( );
                      typename  bolt::cl::device_vector< oType >::pointer mergeResBuffer =  result.getContainer( ).data( );

                      std::merge(&mergeInputBuffer1[first1.m_Index],&mergeInputBuffer1[ last1.m_Index ],
                                      &mergeInputBuffer2[first2.m_Index],&mergeInputBuffer2[ last2.m_Index ],
                                      &mergeResBuffer[result.m_Index],comp);
                        return result + (last1 - first1) + (last2 - first2);


                    }

                }

            }

            // This template is called after we detect random access iterators
            // This is called strictly for iterators that are derived from device_vector< T >::iterator

            template<typename DVInputIterator1,typename DVInputIterator2,typename DVoutputIterator, 
            typename StrictWeakCompare>
            DVoutputIterator merge_pick_iterator(bolt::cl::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                 DVoutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code,
                bolt::cl::fancy_iterator_tag )
            {
                typedef typename std::iterator_traits<DVInputIterator>::value_type iType;
                size_t szElements = static_cast<size_t>(std::distance(first, last) );
                if (szElements == 0)
                    return init;

                bolt::cl::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
                if(runMode == bolt::cl::control::Automatic)
                {
                    runMode = ctl.getDefaultPathToRun();
                }
                
                switch(runMode)
                {
                case bolt::cl::control::OpenCL :
                        return merge_enqueue( ctl, first1, last1,first2, last2, result, comp, cl_code);
              
                case bolt::cl::control::MultiCoreCpu: 
                    #ifdef ENABLE_TBB
                    {
                      return bolt::btbb::merge(first1, last1,first2, last2, result, comp);
                    }
                    #else
                    {
                       throw std::runtime_error( "The MultiCoreCpu version of merge is not enabled to be built! \n" );
                    }
                    #endif

                case bolt::cl::control::SerialCpu: 
                     return std::merge(first1, last1,first2, last2, result, comp);

                default: /* Incase of runMode not set/corrupted */
                    return std::merge(first1, last1,first2, last2, result, comp);

                }

            }

            //----
            // This is the base implementation of reduction that is called by all of the convenience wrappers below.
            // first and last must be iterators from a DeviceVector

            template<typename DVInputIterator1,typename DVInputIterator2,typename DVoutputIterator, 
            typename StrictWeakCompare>
            DVoutputIterator  merge_enqueue(bolt::cl::control &ctl,
                const DVInputIterator1& first1,
                const DVInputIterator1& last1,
                const DVInputIterator2& first2,
                const DVInputIterator2& last2,
                 DVoutputIterator& result,
                const StrictWeakCompare& comp,
                const std::string& cl_code )
            {
                typedef typename std::iterator_traits< DVInputIterator1 >::value_type iType1;
                typedef typename std::iterator_traits< DVInputIterator2 >::value_type iType2;
                typedef typename std::iterator_traits< DVoutputIterator >::value_type rType;

                std::vector<std::string> typeNames( merge_end);
                typeNames[merge_iVType1] = TypeName< iType1 >::get( );
                typeNames[merge_iVType2] = TypeName< iType2 >::get( );
                typeNames[merge_iIterType1] = TypeName< DVInputIterator1 >::get( );
                typeNames[merge_iIterType2] = TypeName< DVInputIterator2 >::get( );
                typeNames[merge_rIterType]= TypeName< DVoutputIterator >::get( );
                typeNames[merge_resType] = TypeName< rType >::get( );
                typeNames[merge_StrictWeakCompare] = TypeName< StrictWeakCompare >::get();

                

                std::vector<std::string> typeDefinitions;
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< iType1 >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< iType2 >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< DVInputIterator1 >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< DVInputIterator2 >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< DVoutputIterator  >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< rType  >::get() )
                PUSH_BACK_UNIQUE( typeDefinitions, ClCode< StrictWeakCompare  >::get() )



                //bool cpuDevice = ctl.device().getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;
                /*\TODO - Do CPU specific kernel work group size selection here*/
                //const size_t kernel0_WgSize = (cpuDevice) ? 1 : WAVESIZE*KERNEL02WAVES;
                std::string compileOptions;
                //std::ostringstream oss;
                //oss << " -DKERNEL0WORKGROUPSIZE=" << kernel0_WgSize;

                Merge_KernelTemplateSpecializer ts_kts;
                std::vector< ::cl::Kernel > kernels = bolt::cl::getKernels(
                    ctl,
                    typeNames,
                    &ts_kts,
                    typeDefinitions,
                    merge_kernels,
                    compileOptions);

                // Set up shape of launch grid and buffers:
                cl_uint computeUnits     = ctl.getDevice().getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
                int wgPerComputeUnit =  ctl.getWGPerComputeUnit();
                size_t numWG = computeUnits * wgPerComputeUnit;

                cl_int l_Error = CL_SUCCESS;

                const size_t wgSize  = kernels[0].getWorkGroupInfo< CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE >(
                    ctl.getDevice( ), &l_Error );

                V_OPENCL( l_Error, "Error querying kernel for CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE" );

                // Create buffer wrappers so we can access the host functors, for read or writing in the kernel
                ALIGNED( 256 ) StrictWeakCompare aligned_merge( comp );
                //::cl::Buffer userFunctor(ctl.context(), CL_MEM_USE_HOST_PTR|CL_MEM_READ_ONLY, sizeof(aligned_merge),
                //  &aligned_merge );
                control::buffPointer userFunctor = ctl.acquireBuffer( sizeof( aligned_merge ),
                    CL_MEM_USE_HOST_PTR|CL_MEM_READ_ONLY, &aligned_merge );


//                control::buffPointer result = ctl.acquireBuffer( sizeof( T ) * numWG,
  //                  CL_MEM_ALLOC_HOST_PTR|CL_MEM_WRITE_ONLY );

                cl_uint szElements1 = static_cast< cl_uint >( first1.distance_to(last1 ) );
                cl_uint szElements2 = static_cast< cl_uint >( first2.distance_to(last2 ) );


                V_OPENCL( kernels[0].setArg(0, first1.getContainer().getBuffer() ), "Error setting kernel argument" );
                V_OPENCL( kernels[0].setArg(1, first1.gpuPayloadSize( ),const_cast<typename DVInputIterator1::Payload*>
                   (&first1.gpuPayload( )) ), "Error setting a kernel argument" );
                V_OPENCL( kernels[0].setArg(2, szElements1), "Error setting kernel argument" );

                V_OPENCL( kernels[0].setArg(3, first2.getContainer().getBuffer() ), "Error setting kernel argument" );
                V_OPENCL( kernels[0].setArg(4, first2.gpuPayloadSize( ),const_cast<typename DVInputIterator2::Payload*>
                    (&first2.gpuPayload( )) ),"Error setting a kernel argument" );
                V_OPENCL( kernels[0].setArg(5, szElements2), "Error setting kernel argument" );

                V_OPENCL( kernels[0].setArg(6, result.getContainer().getBuffer()), "Error setting kernel argument" );
                V_OPENCL( kernels[0].setArg(7, result.gpuPayloadSize( ),const_cast<typename DVoutputIterator::Payload*>
                    (&result.gpuPayload( )) ),"Error setting a kernel argument" );
                V_OPENCL( kernels[0].setArg(8, *userFunctor), "Error setting kernel argument" );
                
        //        ::cl::LocalSpaceArg loc;
         //       loc.size_ = wgSize*sizeof(T);
         //       V_OPENCL( kernels[0].setArg(5, loc), "Error setting kernel argument" );
                
                int leng = szElements1 > szElements2 ? szElements1 : szElements2;
                leng = leng + 64 - (leng % wgSize);

                l_Error = ctl.getCommandQueue().enqueueNDRangeKernel(
                    kernels[0],
                    ::cl::NullRange,
                    ::cl::NDRange(leng),
                    ::cl::NDRange(wgSize));

                V_OPENCL( l_Error, "enqueueNDRangeKernel() failed for merge() kernel" );

                return (result + szElements1 + szElements2);
            }
        }


        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator > 
        OutputIterator merge (InputIterator1 first1, InputIterator1 last1, InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result,const std::string& cl_code)
        {

           typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
            return merge(bolt::cl::control::getDefault(), first1, last1, first2,last2,result, bolt::cl::less<iType1>()
                , cl_code);
        };


        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator > 
        OutputIterator merge (bolt::cl::control &ctl,InputIterator1 first1, InputIterator1 last1,InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result,const std::string& cl_code)
        {
            typedef typename std::iterator_traits<InputIterator1>::value_type iType1;
            return merge(ctl, first1, last1, first2,last2,result, bolt::cl::less<iType1>() , cl_code);
        };



        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator,
            typename StrictWeakCompare > 
        OutputIterator merge (InputIterator1 first1, InputIterator1 last1, InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result,StrictWeakCompare comp,const std::string& cl_code)
        {
            return merge(bolt::cl::control::getDefault(), first1, last1, first2,last2,result, comp, cl_code);
        };


        template<typename InputIterator1 , typename InputIterator2 , typename OutputIterator, 
         typename StrictWeakCompare >  
        OutputIterator merge (bolt::cl::control &ctl,InputIterator1 first1, InputIterator1 last1,InputIterator2 first2, 
        InputIterator2 last2, OutputIterator result,StrictWeakCompare comp,const std::string& cl_code)
        {

            bolt::cl::control::e_RunMode runMode = ctl.getForceRunMode();  // could be dynamic choice some day.
            if(runMode == bolt::cl::control::Automatic)
            {
                  runMode = ctl.getDefaultPathToRun();
            }
            if (runMode == bolt::cl::control::SerialCpu) {
                return std::merge(first1, last1,first2,last2,result, comp);
            } else {

                    return detail::merge_detect_random_access(ctl, first1, last1, first2,last2,result, comp , cl_code,
                               typename std::iterator_traits< InputIterator1 >::iterator_category( ));
            }

        };
 
    }

};






#endif //BOLT_CL_MERGE_INL
