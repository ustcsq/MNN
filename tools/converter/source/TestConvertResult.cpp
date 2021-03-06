//
//  TestConvertResult.cpp
//  MNNConverter
//
//  Created by MNN on 2020/01/22.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "MNN_generated.h"
#include "caffeConverter.hpp"
#include "liteConverter.hpp"
#include "onnxConverter.hpp"
#include "tensorflowConverter.hpp"
#include <MNN/expr/Expr.hpp>
#include <MNN/expr/ExprCreator.hpp>
#include "PostConverter.hpp"
#include "rapidjson/document.h"
#include "options.hpp"
#include <fstream>
#include <sstream>
#include "config.hpp"
#include "common/Global.hpp"
using namespace MNN::Express;
int main(int argc, char *argv[]) {
    if (argc < 3) {
        MNN_ERROR("Usage: ./TestConvertResult [Onnx, Tf] ${Dir}\n");
        return 0;
    }
    std::string inputType = argv[1];
    std::string directName = argv[2];
    auto inputModel = modelConfig::ONNX;
    auto converter = onnx2MNNNet;
    auto suffix = ".onnx";
    auto dataFormat = NCHW;
    if (inputType == "Tf") {
        inputModel = modelConfig::TENSORFLOW;
        converter = tensorflow2MNNNet;
        suffix = ".pb";
        dataFormat = NHWC;
    }
    MNN_PRINT("Test %s\n", directName.c_str());
    std::string defaultCacheFile = ".___temp.mnn";
    {
        modelConfig modelPath;
        modelPath.model = inputModel;
        Global<modelConfig>::Reset(&modelPath);

        auto options = common::DefaultOptions();
        std::ostringstream modelNameOs;
        modelNameOs << directName << "/test" << suffix;
        std::unique_ptr<MNN::NetT> netT = std::unique_ptr<MNN::NetT>(new MNN::NetT());
        converter(modelNameOs.str().c_str(), "Test", options, netT);
        std::unique_ptr<MNN::NetT> newNet = optimizeNet(netT, false);
        flatbuffers::FlatBufferBuilder builderOutput(1024);
        builderOutput.ForceDefaults(true);
        auto len = MNN::Net::Pack(builderOutput, newNet.get());
        builderOutput.Finish(len);
        int sizeOutput    = builderOutput.GetSize();
        auto bufferOutput = builderOutput.GetBufferPointer();

        std::ofstream output(defaultCacheFile.c_str(), std::ofstream::binary);
        output.write((const char*)bufferOutput, sizeOutput);
    }
    rapidjson::Document document;
    std::map<std::string, float> inputInfo;
    std::map<std::string, std::vector<int>> inputShape;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    {
        std::ostringstream jsonNameOs;
        jsonNameOs << directName << "/input.json";
        std::ifstream fileNames(jsonNameOs.str().c_str());
        std::ostringstream output;
        output << fileNames.rdbuf();
        auto outputStr = output.str();
        document.Parse(outputStr.c_str());
        if (document.HasParseError()) {
            MNN_ERROR("Invalid json\n");
            return 0;
        }
        if (document.HasMember("inputs")) {
            auto inputsInfo = document["inputs"].GetArray();
            for (auto iter = inputsInfo.begin(); iter !=inputsInfo.end(); iter++) {
                auto obj = iter->GetObject();
                std::string name = obj["name"].GetString();
                inputNames.emplace_back(name);
                MNN_PRINT("%s\n", name.c_str());
                if (obj.HasMember("value")) {
                    float value = obj["value"].GetFloat();
                    inputInfo.insert(std::make_pair(name, value));
                }
                if (obj.HasMember("shape")) {
                    auto dims = obj["shape"].GetArray();
                    std::vector<int> shapes;
                    for (auto iter = dims.begin(); iter != dims.end(); iter++) {
                        shapes.emplace_back(iter->GetInt());
                    }
                    inputShape.insert(std::make_pair(name, shapes));
                }
            }
        }
        if (document.HasMember("outputs")) {
            auto array = document["outputs"].GetArray();
            for (auto iter = array.begin(); iter !=array.end(); iter++) {
                std::string name = iter->GetString();
                MNN_PRINT("output: %s\n", name.c_str());
                outputNames.emplace_back(name);
            }
        }
    }
    auto varMap = Variable::loadMap(defaultCacheFile.c_str());
    for (auto inputName : inputNames) {
        if (varMap.find(inputName) == varMap.end()) {
            MNN_ERROR("TESTERROR Can't find var: %s\n", inputName.c_str());
            continue;
        }
        // Resize
        auto shapeIter = inputShape.find(inputName);
        if (shapeIter != inputShape.end()) {
            auto s = shapeIter->second;
            varMap[inputName]->resize(s);
        }
        varMap[inputName] = _ChangeInputFormat(varMap[inputName], dataFormat);
#define LOAD_DATA(TYPE)\
    auto ptr = varMap[inputName]->writeMap<TYPE>();\
    if (inputInfo.find(inputName) != inputInfo.end()) {\
        auto value = inputInfo[inputName];\
        for (int i=0; i<info->size; ++i) {\
            ptr[i] = value;\
        }\
    } else {\
        std::ostringstream fileNameOs;\
        fileNameOs << directName << "/" << inputName << ".txt";\
        auto fileName = fileNameOs.str();\
        std::ifstream inputOs(fileName.c_str());\
        if (inputOs.fail()) {\
            MNN_ERROR("TESTERROR Can't open %s\n", fileName.c_str());\
            continue;\
        }\
        for (int i=0; i<info->size; ++i) {\
            inputOs >> ptr[i];\
        }\
    }
        auto info = varMap[inputName]->getInfo();
        if (info->type == halide_type_of<uint8_t>()) {
            LOAD_DATA(uint8_t)
        } else if (info->type == halide_type_of<int32_t>()) {
            LOAD_DATA(uint8_t)
        } else if (info->type == halide_type_of<float>()){
            LOAD_DATA(float)
        } else {
            MNN_ERROR("TESTERROR Not support input type\n");\
        }
#undef LOAD_DATA
    }
    bool modelError = false;
    for (int i=0; i<outputNames.size(); ++i) {
        auto name = outputNames[i];
        if (varMap.find(name) == varMap.end()) {
            MNN_ERROR("TESTERROR, Can't find var: %s\n", name.c_str());
            return 0;
        }
        auto output = varMap[name];
        auto info = output->getInfo();
        auto ptr = output->readMap<float>();
        if (nullptr == info || nullptr == ptr) {
            MNN_ERROR("TESTERROR ptr / info nullptr\n");
            return 0;
        }
        std::ifstream outputOrigin;
        // First find key
        {
            std::ostringstream outputFileOs;
            outputFileOs << directName << "/" << name <<".txt";
            outputOrigin.open(outputFileOs.str().c_str());
        }
        // Second find order
        if (outputOrigin.fail()) {
            std::ostringstream outputFileOs;
            outputFileOs << directName << "/" << i <<".txt";
            outputOrigin.open(outputFileOs.str().c_str());
        }
        if (info->order == NC4HW4) {
            output = _Convert(output, dataFormat);
            info = output->getInfo();
        }
        if (info->type.code != halide_type_float) {
            output = _Cast<float>(output);
            info = output->getInfo();
        }
        auto targetValue = _Input({info->dim}, info->order, info->type);
        auto targetPtr = targetValue->writeMap<float>();
        for (int i=0; i<info->size; ++i) {
            outputOrigin >> targetPtr[i];
        }
        auto absMax = _ReduceMax(_Abs(targetValue), {});
        auto diff = _Abs(targetValue - output);
        auto diffAbsMax = _ReduceMax(diff);
        auto absMaxV = absMax->readMap<float>()[0];
        auto diffAbsMaxV = diffAbsMax->readMap<float>()[0];
        if (absMaxV * 0.01f < diffAbsMaxV) {
            MNN_ERROR("TESTERROR %s value error : absMaxV:%f - DiffMax %f\n", name.c_str(), absMaxV, diffAbsMaxV);
            modelError = true;
        }
    }
    if (modelError) {
        std::vector<VARP> outputs;
        MNN_ERROR("Save mnn result to  .error director\n");
        for (int i=0; i<outputNames.size(); ++i) {
            auto name = outputNames[i];
            auto v = varMap[name];
            auto info = v->getInfo();
            if (info->order == NC4HW4) {
                v = _Convert(v, NCHW);
            }
            if (info->type.code != halide_type_float) {
                v = _Cast<float>(v);
                info = v->getInfo();
            }
            v.fix(VARP::CONSTANT);
            info = v->getInfo();
            std::ofstream _output((".error/" + name + ".txt").c_str());
            auto ptr = v->readMap<float>();
            for (int v=0; v<info->size; ++v) {
                _output << ptr[v] << "\n";
            }
            v->setName(name);
            outputs.emplace_back(v);
        }
        Variable::save(outputs, ".Error.mnn");
        return 0;
    }
    MNN_PRINT("TEST_SUCCESS\n");
    return 0;
}

