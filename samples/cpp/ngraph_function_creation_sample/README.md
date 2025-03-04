# nGraph Function Creation C++ Sample {#openvino_inference_engine_samples_ngraph_function_creation_sample_README}

This sample demonstrates how to execute an synchronous inference using [OpenVINO Model feature](../../../docs/OV_Runtime_UG/model_representation.md) to create a network, which uses weights from LeNet classification network, which is known to work well on digit classification tasks.

The sample supports only single-channel [MNIST database](http://yann.lecun.com/exdb/mnist) images as an input.

You do not need an XML file to create a network. The API of ngraph::Function allows to create a network on the fly from the source code.

nGraph Function Creation C++ Sample demonstrates the following Inference Engine API in your applications:

| Feature    | API  | Description |
|:---        |:---  |:---
|OpenVINO Runtime Version| `ov::get_openvino_versio` | Get Openvino API version
|Available Devices|`ov::Core::get_available_devices`| Get version information of the devices for inference
| Model Operations | `ov::set_batch`, `ov::get_batch` |  Managing of model, operate with its batch size. Setting batch size using input image count.
|nGraph Functions| `ov::op`, `ov::Node`, `ov::Shape::Shape`, `ov::Strides::Strides`, `ov::CoordinateDiff::CoordinateDiff`, `ov::Node::set_friendly_name`, `ov::shape_size`, `ov::ParameterVector::vector`  |  Illustrates how to construct an nGraph function

Basic Inference Engine API is covered by [Hello Classification C++ sample](../hello_classification/README.md).

| Options  | Values |
|:---                              |:---
| Validated Models                 | LeNet
| Model Format                     | Network weights file (\*.bin)
| Validated images                 | single-channel `MNIST ubyte` images
| Supported devices                | [All](../../../docs/OV_Runtime_UG/supported_plugins/Supported_Devices.md) |
| Other language realization       | [Python](../../../samples/python/ngraph_function_creation_sample/README.md) |

## How It Works

At startup, the sample application reads command-line parameters, prepares input data, creates a network using the [OpenVINO Model feature](../../../docs/OV_Runtime_UG/model_representation.md) and passed weights file, loads the network and image(s) to the Inference Engine plugin, performs synchronous inference and processes output data, logging each step in a standard output stream. You can place labels in .labels file near the model to get pretty output.

You can see the explicit description of each sample step at [Integration Steps](../../../docs/OV_Runtime_UG/Integrate_with_customer_application_new_API.md) section of "Integrate the Inference Engine with Your Application" guide.

## Building

To build the sample, please use instructions available at [Build the Sample Applications](../../../docs/OV_Runtime_UG/Samples_Overview.md) section in Inference Engine Samples guide.

## Running

To run the sample, you need specify a model wights and MNIST ubyte image:

- you can use LeNet model weights in the sample folder: `lenet.bin` with FP32 weights file
- you can use images from the media files collection available at https://storage.openvinotoolkit.org/data/test_data.

> **NOTES**:
>
> - The `lenet.bin` with FP32 weights file was generated by the [Model Optimizer](../../../docs/MO_DG/Deep_Learning_Model_Optimizer_DevGuide.md) tool from the public LeNet model with the `--input_shape [64,1,28,28]` parameter specified.
>
> The original model is available in the [Caffe* repository](https://github.com/BVLC/caffe/tree/master/examples/mnist) on GitHub\*.

Running the application with the `-h` option yields the following usage message:

```
ngraph_function_creation_sample -h
[ INFO ] InferenceEngine:
        API version ............<version>
        Build ..................<build>
        Description ....... API
[ INFO ] Parsing input parameters

ngraph_function_creation_sample [OPTION]
Options:

    -h                      Print a usage message.
    -m "<path>"             Required. Path to a .bin file with weights for the trained model.
    -i "<path>"             Required. Path to a folder with images or path to image files. Support ubyte files only.
    -d "<device>"           Optional. Specify the target device to infer on (the list of available devices is shown below). Default value is CPU. Use "-d HETERO:<comma_separated_devices_list>" format to specify HETERO plugin. Sample will look for a suitable plugin for device specified.
    -nt "<integer>"         Number of top results. The default value is 10.

Available target devices:  <devices>

```

Running the application with the empty list of options yields the usage message given above and an error message.

You can do inference of an image using a pre-trained model on a GPU using the following command:

```
<path_to_sample>/ngraph_function_creation_sample -m <path_to_weights_file>/lenet.bin -i <path_to_image> -d GPU
```

## Sample Output

The sample application logs each step in a standard output stream and outputs top-10 inference results.

```
ngraph_function_creation_sample.exe -m lenet.bin -i 7-ubyte
[ INFO ] InferenceEngine:
        API version ............ <version>
        Build .................. <build>
        Description ....... API
[ INFO ] Parsing input parameters
[ INFO ] Files were added: 1
[ INFO ]     7-ubyte
[ INFO ] Loading Inference Engine
[ INFO ] Device info:
        CPU
        openvino_intel_cpu_plugin version ......... <version>
        Build ........... <build>

[ INFO ] Preparing input blobs
[ INFO ] Batch size is 1
[ INFO ] Checking that the outputs are as the sample expects
[ INFO ] Loading model to the device
[ INFO ] Create infer request
[ INFO ] Start inference
[ INFO ] Processing output blobs

Top 10 results:

Image 7-ubyte

classid probability
------- -----------
7       1.0000000
4       0.0000000
8       0.0000000
9       0.0000000
5       0.0000000
3       0.0000000
1       0.0000000
0       0.0000000
2       0.0000000
6       0.0000000

[ INFO ] This sample is an API example, for performance measurements, use the dedicated benchmark_app tool

```

## Deprecation Notice

<table>
  <tr>
    <td><strong>Deprecation Begins</strong></td>
    <td>June 1, 2020</td>
  </tr>
  <tr>
    <td><strong>Removal Date</strong></td>
    <td>December 1, 2020</td>
  </tr>
</table>

*Starting with the OpenVINO™ toolkit 2020.2 release, all of the features previously available through nGraph have been merged into the OpenVINO™ toolkit. As a result, all the features previously available through ONNX RT Execution Provider for nGraph have been merged with ONNX RT Execution Provider for OpenVINO™ toolkit.*

*Therefore, ONNX RT Execution Provider for nGraph will be deprecated starting June 1, 2020 and will be completely removed on December 1, 2020. Users are recommended to migrate to the ONNX RT Execution Provider for OpenVINO™ toolkit as the unified solution for all AI inferencing on Intel® hardware.*

## See Also

- [Integrate the Inference Engine with Your Application](../../../docs/OV_Runtime_UG/Integrate_with_customer_application_new_API.md)
- [Using Inference Engine Samples](../../../docs/OV_Runtime_UG/Samples_Overview.md)
- [Model Optimizer](../../../docs/MO_DG/Deep_Learning_Model_Optimizer_DevGuide.md)
