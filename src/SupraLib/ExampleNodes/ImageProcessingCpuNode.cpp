// ================================================================================================
// 
// If not explicitly stated: Copyright (C) 2018, all rights reserved,
//      Rüdiger Göbl 
//		Email r.goebl@tum.de
//      Chair for Computer Aided Medical Procedures
//      Technische Universität München
//      Boltzmannstr. 3, 85748 Garching b. München, Germany
// 
// ================================================================================================

#include "ImageProcessingCpuNode.h"

#include "USImage.h"
#include <utilities/Logging.h>
#include <torch/script.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <memory>

using namespace std;

namespace supra
{
	ImageProcessingCpuNode::ImageProcessingCpuNode(tbb::flow::graph & graph, const std::string & nodeID, bool queueing)
		: AbstractNode(nodeID, queueing)
		, m_factor(1.0)
	{
		// Create the underlying tbb node for handling the message passing. This usually does not need to be modified.
		if (queueing)
		{
			m_node = unique_ptr<NodeTypeQueueing>(
				new NodeTypeQueueing(graph, 1, [this](shared_ptr<RecordObject> inObj) -> shared_ptr<RecordObject> { return checkTypeAndProcess(inObj); }));
		}
		else
		{
			m_node = unique_ptr<NodeTypeDiscarding>(
				new NodeTypeDiscarding(graph, 1, [this](shared_ptr<RecordObject> inObj) -> shared_ptr<RecordObject> { return checkTypeAndProcess(inObj); }));
		}

		m_callFrequency.setName("ImageProcessingCpuNode");

		// Define the parameters that this node reveals to the user
		m_valueRangeDictionary.set<double>("factor", 0.0, 2.0, 1.0, "Factor");
		m_valueRangeDictionary.set<DataType>("outputType", { TypeFloat, TypeUint8, TypeInt16 }, TypeFloat, "Output type");
		
		// read the configuration to apply the default values
		configurationChanged();
	}

	void ImageProcessingCpuNode::configurationChanged()
	{
		m_factor = m_configurationDictionary.get<double>("factor");
		m_outputType = m_configurationDictionary.get<DataType>("outputType");
	}

	void ImageProcessingCpuNode::configurationEntryChanged(const std::string& configKey)
	{
		// lock the object mutex to make sure no processing happens during parameter changes
		unique_lock<mutex> l(m_mutex);
		if (configKey == "factor")
		{
			m_factor = m_configurationDictionary.get<double>("factor");
		}
		else if (configKey == "outputType")
		{
			m_outputType = m_configurationDictionary.get<DataType>("outputType");
		}
	}

	template <typename InputType, typename OutputType>
	std::shared_ptr<ContainerBase> ImageProcessingCpuNode::processTemplated(std::shared_ptr<const Container<InputType> > imageData, vec3s size)
            {
                    // here the actual processing happens!

                    size_t width = size.x;
                    size_t height = size.y;
                    size_t depth = size.z;

                    // make sure the data is in cpu memory
                    auto inImageData = imageData;
                    if (!inImageData->isHost() && !inImageData->isBoth())
                    {
                            inImageData = make_shared<Container<InputType> >(LocationHost, *inImageData);
                    }
                    // Get pointer to the actual memory block
                    const InputType* pInputImage = inImageData->get();


                // Deserialize the ScriptModule from a file using torch::jit::load().
                    std::shared_ptr<torch::jit::script::Module> module = torch::jit::load("model.pt");
                    assert(module != nullptr);
                    cout << "Model loaded.\n";

                    // Create a vector of inputs.
                    std::vector<torch::jit::IValue> inputs;
                    // reading raw data and saving as a vector
                    std::ifstream fin("rawData_1.raw", std::ios::binary);
                    if(!fin)
                    {
                        std::cout << " Error, Couldn't find the file" << "\n";
                        return 0;
                    }
                    fin.seekg(0, std::ios::end);
                    const size_t num_elements = fin.tellg() / sizeof(signed short);
                    fin.seekg(0, std::ios::beg );
                    vector<signed short > inData(num_elements);
                    fin.read(reinterpret_cast<char*>(&inData[0]), num_elements*sizeof(signed short));
                    //converting input data from vector to torch tensor (remember not at:tensor,but torch::tensor)
                    //auto f = torch::from_blob(a, {17, 64, 128, 2077}).cuda();
                    at::TensorOptions options(at::ScalarType::Byte);
                    auto f = torch::from_blob(inData.data(), {64, 2077, 256,34},options);
                    f = f.toType(torch::kInt16);
                    f=f.permute({3,1,2,0});//now it is the matrix of {34,2077,256,64}
                    //int x=f.size(0);
                    int x=1;//in order to run only one frame
                    int y=f.size(1);//2077
                    int z=f.size(2);//256
                    int w=f.size(3);//64
                    // prepare the output memory
                    auto outImageData = make_shared<Container<OutputType> >(LocationHost, inImageData->getStream(), z*1600*x);
                    // Get pointer to the actual memory block
                    OutputType* pOutputImage = outImageData->get();
                    //if GPU is used, then the following has to be done
                    auto a1=torch::empty({y,z,w});
                    auto a2=torch::empty({z,w});
                    auto a3=torch::empty({1600,z,w});
                    auto a4=torch::empty({w,z,1600});
                    auto a5=torch::empty({1,w,z,1600});
                    for (int64_t i = 0; i < x; i++) {
                        a1 = f[i];//{2077,256,64}
                        for (int64_t j = 0; j < 1600; j++) {
                            a2 = a1[j];
                            a3[j] = a2;//{1600,256,64}

                        }
                        //cudaFree(tensor.storage()->data()); // clearing gpu memory
                        //normalization step
                        a4 = a3.permute({2, 1, 0});//{64,256,1600}
                        a4 = (a4 + 2047) / (2 * 2047);
                        a5 = torch::unsqueeze(a4, 0);
                        cout<<a5.sizes()<<endl;
                        inputs.emplace_back(a5);
                        // Execute the model and turn its output into a tensor.
                        at::Tensor output = module->forward(inputs).toTensor();
                        output =at::squeeze(output);
                        //Efficient access to tensor elements from (https://devhub.io/repos/soumith-ATen)
                        // assert foo is 2-dimensional and holds floats
                        auto foo_a = output.accessor<float,2>();
                        float trace = 0;
                        for(int i = 0; i < foo_a.size(0); i++) {
                            for(int j = 0; j < foo_a.size(1); j++) {
                          // use the accessor foo_a to get tensor data.
                          trace += foo_a[i][i];
                            }
                        }
                        for (int y = 0; y < output.size(1); y++)
                        {
                                for (int x = 0; x < output.size(0); x++)
                                {
                                        // Perform a pixel-wise operatin on the image
                                        WorkType value = foo_a[x][y];

                                        // Get the input pixel value and cast it to out working type.
                                        // As this should in general be a type with wider range / precision, this cast does not loose anything.

                                        // Perform operation, in this case multiplication
                                        // Store the output pixel value..
                                            // This should u in a sane way, that is with clamping. There is a helper for that!
                                        pOutputImage[x + y*output.size(0) + i *output.size(0)*output.size(1)] = clampCast<OutputType>(value);
                                        // Because this is templated, we need to cast from "WorkType" to "OutputType".
                                }
                        }
                    }







		// return the result!
                return outImageData;
	}

	template <typename InputType>
	std::shared_ptr<ContainerBase> ImageProcessingCpuNode::processTemplateSelection(std::shared_ptr<const Container<InputType> > imageData, vec3s size)
	{
		// With the function already templated on the input type, handle the desired output type.
		switch (m_outputType)
		{
		case supra::TypeUint8:
			return processTemplated<InputType, uint8_t>(imageData, size);
			break;
		case supra::TypeInt16:
			return processTemplated<InputType, int16_t>(imageData, size);
			break;
		case supra::TypeFloat:
			return processTemplated<InputType, float>(imageData, size);
			break;
		default:
			logging::log_error("ImageProcessingCpuNode: Output image type not supported");
			break;
		}
		return nullptr;
	}

	shared_ptr<RecordObject> ImageProcessingCpuNode::checkTypeAndProcess(shared_ptr<RecordObject> inObj)
	{
		shared_ptr<USImage> pImage = nullptr;
		if (inObj && inObj->getType() == TypeUSImage)
		{
			shared_ptr<USImage> pInImage = dynamic_pointer_cast<USImage>(inObj);
			if (pInImage)
			{
				// lock the object mutex to make sure no parameters are changed during processing
				unique_lock<mutex> l(m_mutex);
				m_callFrequency.measure();

				std::shared_ptr<ContainerBase> pImageProcessedData;

				// The input and output types have to be determined dynamically. We do this in to stages of templated functions.
				// This first switch handles the different input data types. There is no need to support all types, 
				// only those meaningful for the operation of the node.
				switch (pInImage->getDataType())
				{
				case TypeUint8:
					pImageProcessedData = processTemplateSelection<uint8_t>(pInImage->getData<uint8_t>(), pInImage->getSize());
					break;
				case TypeInt16:
					pImageProcessedData = processTemplateSelection<int16_t>(pInImage->getData<int16_t>(), pInImage->getSize());
					break;
				case TypeFloat:
					pImageProcessedData = processTemplateSelection<float>(pInImage->getData<float>(), pInImage->getSize());
					break;
				default:
					logging::log_error("ImageProcessingCpuNode: Input image type not supported");
					break;
				}
				m_callFrequency.measureEnd();

				// Wrap the returned Container in an USImage with the same size etc.
				pImage = make_shared<USImage>(
					pInImage->getSize(),
					pImageProcessedData,
					pInImage->getImageProperties(),
					pInImage->getReceiveTimestamp(),
					pInImage->getSyncTimestamp());
			}
			else {
				logging::log_error("ImageProcessingCpuNode: could not cast object to USImage type, is it in suppored ElementType?");
			}
		}
		return pImage;
	}
}
