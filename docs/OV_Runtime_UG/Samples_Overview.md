# Inference Engine Samples {#openvino_docs_IE_DG_Samples_Overview}

@sphinxdirective

.. _code samples:

.. toctree::
   :maxdepth: 1
   :hidden:
   
   openvino_inference_engine_samples_classification_sample_async_README
   openvino_inference_engine_ie_bridges_python_sample_classification_sample_async_README
   openvino_inference_engine_samples_hello_classification_README
   openvino_inference_engine_ie_bridges_c_samples_hello_classification_README
   openvino_inference_engine_ie_bridges_python_sample_hello_classification_README
   openvino_inference_engine_samples_hello_reshape_ssd_README
   openvino_inference_engine_ie_bridges_python_sample_hello_reshape_ssd_README
   openvino_inference_engine_samples_hello_nv12_input_classification_README
   openvino_inference_engine_ie_bridges_c_samples_hello_nv12_input_classification_README
   openvino_inference_engine_samples_hello_query_device_README
   openvino_inference_engine_ie_bridges_python_sample_hello_query_device_README
   openvino_inference_engine_samples_ngraph_function_creation_sample_README
   openvino_inference_engine_ie_bridges_python_sample_ngraph_function_creation_sample_README
   openvino_inference_engine_samples_speech_sample_README
   openvino_inference_engine_ie_bridges_python_sample_speech_sample_README
   openvino_inference_engine_samples_benchmark_app_README
   openvino_inference_engine_tools_benchmark_tool_README

@endsphinxdirective

The Inference Engine sample applications are simple console applications that show how to utilize specific Inference Engine capabilities within an application, assist developers in executing specific tasks such as loading a model, running inference, querying specific device capabilities and etc.

After installation of Intel® Distribution of OpenVINO™ toolkit, С, C++ and Python* sample applications are available in the following directories, respectively:
* `<INSTALL_DIR>/samples/c`
* `<INSTALL_DIR>/samples/cpp`
* `<INSTALL_DIR>/samples/python`

Inference Engine sample applications include the following:

- **Speech Sample** - Acoustic model inference based on Kaldi neural networks and speech feature vectors.
   - [Automatic Speech Recognition C++ Sample](../../samples/cpp/speech_sample/README.md)
   - [Automatic Speech Recognition Python Sample](../../samples/python/speech_sample/README.md)
- **Benchmark Application** – Estimates deep learning inference performance on supported devices for synchronous and asynchronous modes.
   - [Benchmark C++ Tool](../../samples/cpp/benchmark_app/README.md)
   - [Benchmark Python Tool](../../tools/benchmark_tool/README.md)
- **Hello Classification Sample** – Inference of image classification networks like AlexNet and GoogLeNet using Synchronous Inference Request API. Input of any size and layout can be set to an infer request which will be pre-processed automatically during inference (the sample supports only images as inputs and supports Unicode paths).
   - [Hello Classification C++ Sample](../../samples/cpp/hello_classification/README.md)
   - [Hello Classification C Sample](../../samples/c/hello_classification/README.md)
   - [Hello Classification Python Sample](../../samples/python/hello_classification/README.md)
- **Hello NV12 Input Classification Sample** – Input of any size and layout can be provided to an infer request. The sample transforms the input to the NV12 color format and pre-process it automatically during inference. The sample supports only images as inputs.
   - [Hello NV12 Input Classification C++ Sample](../../samples/cpp/hello_nv12_input_classification/README.md)
   - [Hello NV12 Input Classification C Sample](../../samples/c/hello_nv12_input_classification/README.md)
- **Hello Query Device Sample** – Query of available Inference Engine devices and their metrics, configuration values.
   - [Hello Query Device C++ Sample](../../samples/cpp/hello_query_device/README.md)
   - [Hello Query Device Python* Sample](../../samples/python/hello_query_device/README.md)
- **Hello Reshape SSD Sample** – Inference of SSD networks resized by ShapeInfer API according to an input size.
   - [Hello Reshape SSD C++ Sample**](../../samples/cpp/hello_reshape_ssd/README.md)
   - [Hello Reshape SSD Python Sample**](../../samples/python/hello_reshape_ssd/README.md)
- **Image Classification Sample Async** – Inference of image classification networks like AlexNet and GoogLeNet using Asynchronous Inference Request API (the sample supports only images as inputs).
   - [Image Classification Async C++ Sample](../../samples/cpp/classification_sample_async/README.md)
   - [Image Classification Async Python* Sample](../../samples/python/classification_sample_async/README.md)
- **nGraph Function Creation Sample** – Construction of the LeNet network using the nGraph function creation sample.
   - [nGraph Function Creation C++ Sample](../../samples/cpp/ngraph_function_creation_sample/README.md)
   - [nGraph Function Creation Python Sample](../../samples/python/ngraph_function_creation_sample/README.md)
 
> **NOTE**: All C++ samples support input paths containing only ASCII characters, except the Hello Classification Sample, that supports Unicode.

## Media Files Available for Samples

To run the sample applications, you can use images and videos from the media files collection available at https://storage.openvinotoolkit.org/data/test_data.

## Samples that Support Pre-Trained Models

To run the sample, you can use [public](@ref omz_models_group_public) or [Intel's](@ref omz_models_group_intel) pre-trained models from the Open Model Zoo. The models can be downloaded using the [Model Downloader](@ref omz_tools_downloader).

## Build the Sample Applications

### <a name="build_samples_linux"></a>Build the Sample Applications on Linux*

The officially supported Linux* build environment is the following:

* Ubuntu* 18.04 LTS 64-bit or CentOS* 7 64-bit
* GCC* 7.5.0 (for Ubuntu* 18.04) or GCC* 4.8.5 (for CentOS* 7.6)
* CMake* version 3.10 or higher

> **NOTE**: For building samples from the open-source version of OpenVINO™ toolkit, see the [build instructions on GitHub](https://github.com/openvinotoolkit/openvino/wiki/BuildingCode).

To build the C or C++ sample applications for Linux, go to the `<INSTALL_DIR>/samples/c` or `<INSTALL_DIR>/samples/cpp` directory, respectively, and run the `build_samples.sh` script:
```sh
build_samples.sh
```

Once the build is completed, you can find sample binaries in the following folders:
* C samples: `~/inference_engine_c_samples_build/intel64/Release`
* C++ samples: `~/inference_engine_cpp_samples_build/intel64/Release`

You can also build the sample applications manually:

> **NOTE**: If you have installed the product as a root user, switch to root mode before you continue: `sudo -i`

1. Navigate to a directory that you have write access to and create a samples build directory. This example uses a directory named `build`:
```sh
mkdir build
```
> **NOTE**: If you ran the Image Classification verification script during the installation, the C++ samples build directory was already created in your home directory: `~/inference_engine_samples_build/`

2. Go to the created directory:
```sh
cd build
```

3. Run CMake to generate the Make files for release or debug configuration. For example, for C++ samples:
  - For release configuration:
  ```sh
  cmake -DCMAKE_BUILD_TYPE=Release <INSTALL_DIR>/samples/cpp
  ```
  - For debug configuration:
  ```sh
  cmake -DCMAKE_BUILD_TYPE=Debug <INSTALL_DIR>/samples/cpp
  ```
4. Run `make` to build the samples:
```sh
make
```

For the release configuration, the sample application binaries are in `<path_to_build_directory>/intel64/Release/`;
for the debug configuration — in `<path_to_build_directory>/intel64/Debug/`.

### <a name="build_samples_windows"></a>Build the Sample Applications on Microsoft Windows* OS

The recommended Windows* build environment is the following:
* Microsoft Windows* 10
* Microsoft Visual Studio* 2017, or 2019
* CMake* version 3.10 or higher

> **NOTE**: If you want to use Microsoft Visual Studio 2019, you are required to install CMake 3.14.

To build the C or C++ sample applications on Windows, go to the `<INSTALL_DIR>\samples\c` or `<INSTALL_DIR>\samples\cpp` directory, respectively, and run the `build_samples_msvc.bat` batch file:
```sh
build_samples_msvc.bat
```

By default, the script automatically detects the highest Microsoft Visual Studio version installed on the machine and uses it to create and build
a solution for a sample code. Optionally, you can also specify the preferred Microsoft Visual Studio version to be used by the script. Supported
versions are `VS2017` and `VS2019`. For example, to build the C++ samples using the Microsoft Visual Studio 2017, use the following command:
```sh
<INSTALL_DIR>\samples\cpp\build_samples_msvc.bat VS2017
```

Once the build is completed, you can find sample binaries in the following folders:
* C samples: `C:\Users\<user>\Documents\Intel\OpenVINO\inference_engine_c_samples_build\intel64\Release`
* C++ samples: `C:\Users\<user>\Documents\Intel\OpenVINO\inference_engine_cpp_samples_build\intel64\Release`

You can also build a generated solution manually. For example, if you want to build C++ sample binaries in Debug configuration, run the appropriate version of the
Microsoft Visual Studio and open the generated solution file from the `C:\Users\<user>\Documents\Intel\OpenVINO\inference_engine_cpp_samples_build\Samples.sln`
directory.

### <a name="build_samples_macos"></a>Build the Sample Applications on macOS*

The officially supported macOS* build environment is the following:

* macOS* 10.15 64-bit
* Clang* compiler from Xcode* 10.1 or higher
* CMake* version 3.13 or higher

> **NOTE**: For building samples from the open-source version of OpenVINO™ toolkit, see the [build instructions on GitHub](https://github.com/openvinotoolkit/openvino/wiki/BuildingCode).

To build the C or C++ sample applications for macOS, go to the `<INSTALL_DIR>/samples/c` or `<INSTALL_DIR>/samples/cpp` directory, respectively, and run the `build_samples.sh` script:
```sh
build_samples.sh
```

Once the build is completed, you can find sample binaries in the following folders:
* C samples: `~/inference_engine_c_samples_build/intel64/Release`
* C++ samples: `~/inference_engine_cpp_samples_build/intel64/Release`

You can also build the sample applications manually:

> **NOTE**: If you have installed the product as a root user, switch to root mode before you continue: `sudo -i`

> **NOTE**: Before proceeding, make sure you have OpenVINO™ environment set correctly. This can be done manually by
```sh
cd <INSTALL_DIR>/bin
source setupvars.sh
```

1. Navigate to a directory that you have write access to and create a samples build directory. This example uses a directory named `build`:
```sh
mkdir build
```
> **NOTE**: If you ran the Image Classification verification script during the installation, the C++ samples build directory was already created in your home directory: `~/inference_engine_samples_build/`

2. Go to the created directory:
```sh
cd build
```

3. Run CMake to generate the Make files for release or debug configuration. For example, for C++ samples:
  - For release configuration:
  ```sh
  cmake -DCMAKE_BUILD_TYPE=Release <INSTALL_DIR>/samples/cpp
  ```
  - For debug configuration:
  ```sh
  cmake -DCMAKE_BUILD_TYPE=Debug <INSTALL_DIR>/samples/cpp
  ```
4. Run `make` to build the samples:
```sh
make
```

For the release configuration, the sample application binaries are in `<path_to_build_directory>/intel64/Release/`;
for the debug configuration — in `<path_to_build_directory>/intel64/Debug/`.

## Get Ready for Running the Sample Applications

### Get Ready for Running the Sample Applications on Linux*

Before running compiled binary files, make sure your application can find the
Inference Engine and OpenCV libraries.
Run the `setupvars` script to set all necessary environment variables:
```sh
source <INSTALL_DIR>/setupvars.sh
```

**(Optional)**: The OpenVINO environment variables are removed when you close the
shell. As an option, you can permanently set the environment variables as follows:

1. Open the `.bashrc` file in `<user_home_directory>`:
```sh
vi <user_home_directory>/.bashrc
```

2. Add this line to the end of the file:
```sh
source /opt/intel/openvino_2022/setupvars.sh
```

3. Save and close the file: press the **Esc** key, type `:wq` and press the **Enter** key.
4. To test your change, open a new terminal. You will see `[setupvars.sh] OpenVINO environment initialized`.

You are ready to run sample applications. To learn about how to run a particular
sample, read the sample documentation by clicking the sample name in the samples
list above.

### Get Ready for Running the Sample Applications on Windows*

Before running compiled binary files, make sure your application can find the
Inference Engine and OpenCV libraries.
Use the `setupvars` script, which sets all necessary environment variables:
```sh
<INSTALL_DIR>\setupvars.bat
```

To debug or run the samples on Windows in Microsoft Visual Studio, make sure you
have properly configured **Debugging** environment settings for the **Debug**
and **Release** configurations. Set correct paths to the OpenCV libraries, and
debug and release versions of the Inference Engine libraries.
For example, for the **Debug** configuration, go to the project's
**Configuration Properties** to the **Debugging** category and set the `PATH`
variable in the **Environment** field to the following:

```sh
PATH=<INSTALL_DIR>\runtime\bin;<INSTALL_DIR>\opencv\bin;%PATH%
```
where `<INSTALL_DIR>` is the directory in which the OpenVINO toolkit is installed.

You are ready to run sample applications. To learn about how to run a particular
sample, read the sample documentation by clicking the sample name in the samples
list above.

## See Also
* [Inference Engine Developer Guide](Deep_Learning_Inference_Engine_DevGuide.md)
