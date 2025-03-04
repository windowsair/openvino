# Copyright (C) 2018-2022 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import os
import cv2
import re
import numpy as np
from collections import defaultdict
from pathlib import Path

from openvino.runtime import Tensor, PartialShape
from openvino.runtime.utils.types import get_dtype

from .constants import IMAGE_EXTENSIONS, BINARY_EXTENSIONS
from .logging import logger


class DataQueue:
    def __init__(self, input_data: dict, batch_sizes: list):
        self.input_data = input_data
        self.sizes_map = {}
        for name, tensors in input_data.items():
            self.sizes_map[name] = len(tensors)
        self.index_map = defaultdict.fromkeys(input_data.keys(), 0)
        self.batch_sizes = batch_sizes
        self.size = len(batch_sizes)
        self.current_group_id = 0

    def get_next_input(self):
        data = {}
        for input_name, input_tensors in self.input_data.items():
            data[input_name] = input_tensors[self.index_map[input_name]]
            self.index_map[input_name] = (self.index_map[input_name] + 1) % self.sizes_map[input_name]
        self.current_group_id = (self.current_group_id + 1) % self.size
        return data

    def get_next_batch_size(self):
        return self.batch_sizes[self.current_group_id]


def get_group_batch_sizes(app_input_info):
    batch_sizes = []
    niter = max(len(info.shapes) for info in app_input_info)
    for i in range(niter):
        batch_size = 0
        for info in app_input_info:
            batch_index = info.layout.get_index_by_name('N') if info.layout.has_name('N') else -1
            if batch_index != -1:
                shape = info.shapes[i % len(info.shapes)]
                if batch_size == 0:
                    batch_size = shape[batch_index]
                elif batch_size != shape[batch_index]:
                    raise Exception("Can't deterimine batch size: batch is different for different inputs!")
        if batch_size == 0:
            batch_size = 1
        batch_sizes.append(batch_size)
    return batch_sizes


def get_batch_sizes_per_input_map(app_input_info):
    batch_sizes_map = {}
    for info in app_input_info:
        if info.layout.has_name('N'):
            if info.is_dynamic:
                batch_sizes_map[info.name] = info.getDimentionsByLayout('N')
            else:
                batch_sizes_map[info.name] = [len(info.getDimentionByLayout('N'))]
        else:
            batch_sizes_map[info.name] = [1] * len(info.shapes)
    return batch_sizes_map


def get_input_data(paths_to_input, app_input_info):
    image_mapping, binary_mapping = get_input_file_mappings(paths_to_input, app_input_info)

    image_sizes = get_image_sizes(app_input_info)
    batch_sizes_map = get_batch_sizes_per_input_map(app_input_info)

    images_to_be_used_map = {input_name: len(images) for input_name, images in image_mapping.items()}
    binaries_to_be_used_map = {input_name: len(binaries) for input_name, binaries in binary_mapping.items()}

    for info in app_input_info:
        if info.shapes:
            total_frames = np.sum(batch_sizes_map[info.name])
            if info.name in image_mapping:
                if images_to_be_used_map[info.name] > total_frames and images_to_be_used_map[info.name] % total_frames != 0:
                    images_to_be_used_map[info.name] = images_to_be_used_map[info.name] - images_to_be_used_map[info.name] % total_frames
                    logger.warning(f"Number of provided images for input '{info.name}' is not a multiple of the number of "
                                   f"provided data shapes. Only {images_to_be_used_map[info.name]} images will be processed for this input.")
                elif images_to_be_used_map[info.name] < total_frames:
                    logger.warning(f"Some images will be dublicated: {total_frames} is required, "
                                   f"but only {images_to_be_used_map[info.name]} were provided.")
            elif info.name in binary_mapping:
                if binaries_to_be_used_map[info.name] > total_frames and binaries_to_be_used_map[info.name] % total_frames != 0:
                    binaries_to_be_used_map[info.name] = binaries_to_be_used_map - binaries_to_be_used_map % total_frames
                    logger.warning(f"Number of provided binaries for input '{info.name}' is not a multiple of the number of "
                                   f"provided data shapes. Only {binaries_to_be_used_map[info.name]} binaries will be processed for this input.")
                elif binaries_to_be_used_map[info.name] < total_frames:
                    logger.warning(f"Some binaries will be dublicated: {total_frames} is required, "
                                   f"but only {images_to_be_used_map[info.name]} were provided.")
            else:
                if not (info.is_image_info and len(image_sizes) == 1):
                    logger.warning(f"No input files were given for input '{info.name}'!. This input will be filled with random values!")
        else:
            if info.name in image_mapping:
                logger.info(f"Images given for input '{info.name}' will be processed with original shapes.")
            else:
                raise Exception(f"Input {info.name} is dynamic. Provide data shapes!")

    data = {}
    for port, info in enumerate(app_input_info):
        if info.name in image_mapping:
            data[port] = get_image_tensors(image_mapping[info.name][:images_to_be_used_map[info.name]], info, batch_sizes_map[info.name])

        elif info.name in binary_mapping:
            data[port] = get_binary_tensors(binary_mapping[info.name][:binaries_to_be_used_map[info.name]], info, batch_sizes_map[info.name])

        elif info.is_image_info and len(image_sizes) == 1:
            image_size = image_sizes[0]
            logger.info(f"Create input tensors for input '{info.name}' with image sizes: {image_size}")
            data[port] = get_image_info_tensors(image_size, info)
        else:
            logger.info(f"Fill input '{info.name}' with random values ")
            data[port] = fill_tensors_with_random(info)

    return DataQueue(data, get_group_batch_sizes(app_input_info))


def get_image_tensors(image_paths, info, batch_sizes):
    processed_frames = 0
    widthes = info.widthes if info.is_dynamic else [info.width]
    heights = info.heights if info.is_dynamic else [info.height]
    tensors = []
    process_with_original_shapes = False
    num_shapes = len(info.shapes)
    if num_shapes == 0:
        process_with_original_shapes = True
    num_images = len(image_paths)
    niter = max(num_shapes, num_images)
    for i in range(niter):
        shape = list(info.shapes[i % num_shapes]) if num_shapes else []
        dtype = get_dtype(info.element_type)
        images = np.ndarray(shape=shape, dtype=dtype)
        image_index = processed_frames
        current_batch_size = 1 if process_with_original_shapes else batch_sizes[i % num_shapes]
        for b in range(current_batch_size):
            image_index %= num_images
            image_filename = image_paths[image_index]
            logger.info(f'Prepare image {image_filename}')
            image = cv2.imread(image_filename)
            if process_with_original_shapes:
                logger.info(f'Image will be processed with original shape - {image.shape[:-1]}')
            elif info.layout.has_name('H') and info.layout.has_name('W'):
                new_im_size = (widthes[i % num_shapes], heights[i % num_shapes])
                if image.shape[:-1] != new_im_size:
                    logger.warning(f"Image is resized from ({image.shape[:-1]}) to ({new_im_size})")
                    image = cv2.resize(image, new_im_size)

            if info.scale.size or info.mean.size:
                blue, green, red = cv2.split(image)
                if info.mean.size:
                    blue = np.subtract(blue, info.mean[0])
                    green = np.subtract(green, info.mean[1])
                    red = np.subtract(red, info.mean[2])
                if info.scale.size:
                    blue = np.divide(blue, info.scale[0])
                    green = np.divide(green, info.scale[1])
                    red = np.divide(red, info.scale[2])
                image = cv2.merge([blue, green, red])

            if str(info.layout) in ['[N,C,H,W]', '[C,H,W]']:
                image = image.transpose((2, 0, 1))

            if process_with_original_shapes:
                if len(info.partial_shape) == 4:
                    image = np.expand_dims(image, 0)
                p_shape = PartialShape(image.shape)
                if info.partial_shape.compatible(p_shape):
                    info.data_shapes.append(p_shape.to_shape())
                else:
                    raise Exception(f"Data shape '{str(p_shape)}' provided for input '{info.name}' "
                                    f"is not compatible with partial shape '{str(info.partial_shape)}' for this input.")
                tensors.append(Tensor(image.astype(dtype)))
            else:
                try:
                    images[b] = image
                except ValueError:
                    raise Exception(f"Image shape {image.shape} is not compatible with input shape {shape}! "
                                    f"Make sure -i parameter is valid.")
            image_index += 1
        processed_frames += current_batch_size
        if not process_with_original_shapes:
            tensors.append(Tensor(images))
    return tensors


def get_binary_tensors(binary_paths, info, batch_sizes):
    num_shapes = len(info.shapes)
    num_binaries = len(binary_paths)
    niter = max(num_shapes, num_binaries)
    processed_frames = 0
    tensors = []
    for i in range(niter):
        shape_id = i % num_shapes
        dtype = get_dtype(info.element_type)
        shape = list(info.shapes[shape_id])
        binaries = np.ndarray(shape=shape, dtype=dtype)
        if info.layout.has_name('N'):
            shape[info.layout.get_index_by_name('N')] = 1
        binary_index = processed_frames
        current_batch_size = batch_sizes[shape_id]
        for b in range(current_batch_size):
            binary_index %= num_binaries
            binary_filename = binary_paths[binary_index]
            logger.info("Prepare binary file " + binary_filename)

            binary_file_size = os.path.getsize(binary_filename)
            blob_size = dtype().nbytes * int(np.prod(shape))
            if blob_size != binary_file_size:
                raise Exception(
                    f"File {binary_filename} contains {binary_file_size} bytes but network expects {blob_size}")
            binaries[b] = np.reshape(np.fromfile(binary_filename, dtype), shape)

            binary_index += 1
        processed_frames += current_batch_size
        tensors.append(Tensor(binaries))
    return tensors


def get_image_sizes(app_input_info):
    image_sizes = []
    for info in app_input_info:
        if info.is_image:
            if info.is_static:
                image_sizes.append((info.width, info.height))
            else:
                info_image_sizes = []
                for w, h in zip(info.widthes, info.heights):
                    info_image_sizes.append((w, h))
                image_sizes.append(info_image_sizes)
    return image_sizes


def get_image_info_tensors(image_sizes, layer):
    im_infos = []
    for shape, image_size in zip(layer.shapes, image_sizes):
        im_info = np.ndarray(shape, dtype=get_dtype(layer.element_type))
        for b in range(shape[0]):
            for i in range(shape[1]):
                im_info[b][i] = image_size if i in [0, 1] else 1
        im_infos.append(Tensor(im_info))
    return im_infos


def fill_tensors_with_random(layer):
    dtype = get_dtype(layer.element_type)
    rand_min, rand_max = (0, 1) if dtype == np.bool else (np.iinfo(np.uint8).min, np.iinfo(np.uint8).max)
    # np.random.uniform excludes high: add 1 to have it generated
    if np.dtype(dtype).kind in ['i', 'u', 'b']:
        rand_max += 1
    rs = np.random.RandomState(np.random.MT19937(np.random.SeedSequence(0)))
    input_tensors = []
    for shape in layer.shapes:
        if shape:
            input_tensors.append(Tensor(rs.uniform(rand_min, rand_max, list(shape)).astype(dtype)))
        else:
            input_tensors.append(Tensor(rs.uniform(rand_min, rand_max)))
    return input_tensors


def get_input_file_mappings(paths_to_inputs, app_input_info):
    image_dicts_list = []
    binary_dicts_list = []
    for path in paths_to_inputs:
        image_dict, binary_dict = parse_path(path, app_input_info)
        image_dicts_list.append(image_dict)
        binary_dicts_list.append(binary_dict)

    def merge_dicts(dicts_list):
        merged = defaultdict(list)
        for dict in dicts_list:
            for k,v in dict.items():
                merged[k] += v
        return merged

    def remove_empty_items(dict):
        return {k: sorted(v) for k,v in dict.items() if v}

    return remove_empty_items(merge_dicts(image_dicts_list)), remove_empty_items(merge_dicts(binary_dicts_list))


def parse_path(path, app_input_info):
    """
    Parse "input_1:file1/dir1,file2/dir2,input_2:file3/dir3 or file1/dir1,file2/dir2" into two dicts - with binary files and with images
    """
    input_names = list(info.name for info in app_input_info)
    input_node_names = list(info.node_name for info in app_input_info)
    parsed_names = re.findall(r"([^,]\w+):", path)
    wrong_names = list(name for name in parsed_names if name not in input_names + input_node_names)
    if wrong_names:
        raise Exception(
            f"Wrong input mapping! Cannot find inputs: {wrong_names}. "
            f"Available inputs: {input_names}. "
            "Please check `-i` input data"
        )
    tensor_names = [parsed_name if parsed_name in input_names else input_names[input_node_names.index(parsed_name)] for parsed_name in parsed_names]
    input_pathes = [path for path in re.split(r"[^,]\w+:", path) if path]
    input_path_mapping = defaultdict(list)
    # input mapping is used
    if tensor_names:
        input_path_mapping = {input_: files.strip(",").split(",") for input_, files in zip(tensor_names, input_pathes)}
    else:
        input_files = list()
        _input_pathes = input_pathes[0].strip(",").split(",")
        for _input_path in _input_pathes:
            input_path = Path(_input_path)
            if input_path.exists():
                if input_path.is_dir():
                    input_files += list(str(file_path) for file_path in input_path.iterdir())
                elif input_path.is_file:
                    input_files.append(str(input_path))
            else:
                raise Exception(f"Path '{str(input_path)}' doesn't exist \n {str(input_path)}")
        num_files, num_inputs = len(input_files), len(app_input_info)
        if num_inputs > 1:
            logger.warning(f"Model has {num_inputs} inputs. It's recommended to use name mapping to specify parameters for each input.")
        if num_files > num_inputs and num_files % num_inputs != 0:
            input_files = input_files[:num_files - num_files % num_inputs]
            logger.warning(f"Number of provided input files '{num_files}' is not a multiple of the number of "
                                   f"model inputs. Only {len(input_files)} files fill be used.")
        num_files = len(input_files)
        inputs_to_fill = list(info.name for info in app_input_info if not info.is_image_info)
        for i in range(num_files):
            input_path_mapping[inputs_to_fill[i % len(inputs_to_fill)]].append(input_files[i])

    images_mapping = defaultdict(list)
    binary_mapping = defaultdict(list)
    unsupported_files = list()
    for input_name, _input_pathes in input_path_mapping.items():
        for _input_path in _input_pathes:
            input_path = Path(_input_path)
            if input_path.exists():
                files = list()
                if input_path.is_dir():
                    files = input_path.iterdir()
                elif input_path.is_file:
                    files = [input_path]
                for file in files:
                        if file.suffix.lower() in IMAGE_EXTENSIONS:
                            images_mapping[input_name].append(str(file))
                        elif file.suffix.lower() in BINARY_EXTENSIONS:
                            binary_mapping[input_name].append(str(file))
                        else:
                            unsupported_files.append(str(file))
            else:
                raise Exception(f"Path for input '{input_name}' doesn't exist \n {str(input_path)}")
    if unsupported_files:
        logger.warning(f"This files has unsupported extensions and will be ignored: {unsupported_files}.\n"
            f"Supported extentions:\nImages: {IMAGE_EXTENSIONS}\nBinary: {BINARY_EXTENSIONS}")
    return images_mapping, binary_mapping
