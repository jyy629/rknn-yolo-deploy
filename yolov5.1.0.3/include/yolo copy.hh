#ifndef PSQ_YOLO_HH
#define PSQ_YOLO_HH
enum class YoloModelType
{
    YOLO_V5,
    YOLO_V8,
    YOLO_V26
};
#include <rknpu2/rknn_api.h>
#include <string.h>
#include <iostream>
#include <vector>
#include <pthread.h>
#include <math.h>

struct yolov5_primary_box
{
    const float aw[3][3] = {{10,16,33},{30,62,59},{116,156,373}};
    const float ah[3][3] = {{13,30,23},{61,45,119},{90,198,326}};
};

struct yolovx_primary_box
{

};
struct YoloModelInfo
{
    YoloModelType model_type;
    int inmodel_num;//模型有几个输入
    int outmodel_num;//模型有几个输出
    std::vector<rknn_tensor_attr> input_tensor;
    std::vector<rknn_tensor_attr> output_tensor;
    std::vector<rknn_tensor_mem*> input_mem;
    std::vector<rknn_tensor_mem*> output_mem;
    union
    {
        yolov5_primary_box* yoloov5;//yolov5模型的预选框
        yolovx_primary_box* yoloovx;//后续如果有别的模型，这里可以扩展
        void* userptr;//用户自定义指针，用于存储其他信息
    };
    

};


struct modle_run_document{
    std::vector<pthread_t> pthread_id;//线程id
    float dequant_table[256];//模型经过了sigmoid，没必要再desigmoid
    float de_quant_sigomid_table[256];//模型未经过sigmoid，需要dequantiza+desigmoid
};
//class-------------------------------------------------------------------------------------------

class PSQ_YOLO
{
public:
    PSQ_YOLO(
        rknn_tensor_attr* ex_input_attr, rknn_tensor_attr* ex_output_attr, 
        rknn_tensor_mem** ex_input_mem, rknn_tensor_mem** ex_output_mem, 
        YoloModelType model_type, 
        bool is_sigmoid = true
    )
    // rknn_tensor_attr* ex_input_attr/ex_output_attr（为外部的输入输出rknn_tensor_attr<结构体数组>，接收为一级指针）
    // rknn_tensor_mem** ex_input_mem/ex_output_mem（为外部的输入输出rknn_tensor_mem<结构体指针数组>，接收为二级指针）
    {
        //读取外部参数赋值+初始化类变量

        int ret = init(
            ex_input_attr,ex_output_attr,
            ex_input_mem,ex_output_mem,
            model_type,
            is_sigmoid
        );
        if(ret != 0)
        {
            printf("init failed\n");
            return;
        }




    }





    ~PSQ_YOLO()
    {
        if(model_info.yoloov5 != nullptr){
            delete model_info.yoloov5;
            model_info.yoloov5 = nullptr;
        }
    }

private:

//rknn模型相关变量-------------------------------------------------------------------------------------------
    YoloModelInfo model_info;
    modle_run_document mod_run_doc;


//class——yolo内部相关变量-------------------------------------------------------------------------------------------

//class——yolo内部相关函数-------------------------------------------------------------------------------------------
    int pthread_init()
    {

    }





    //初始化模型类型（v5，v8，v26）
    int init(
        rknn_tensor_attr* ex_input_attr, rknn_tensor_attr* ex_output_attr, 
        rknn_tensor_mem** ex_input_mem, rknn_tensor_mem** ex_output_mem, 
        YoloModelType model_type, 
        bool is_sigmoid = true
    )
    {
        switch(model_type)
        {
            case YoloModelType::YOLO_V5:
                printf("YOLO_V5\n");
                //info:
                this->model_info.model_type = model_type;
                this->model_info.inmodel_num = 1;//初始化输入模型数量为外部参数
                this->model_info.outmodel_num = 3;//初始化输出模型数量为外部参数
                this->model_info.yoloov5 = new yolov5_primary_box();
                //run_document:
                this->mod_run_doc.pthread_id.resize(this->model_info.outmodel_num);//根据外部模型有几个输出调节pthread_id容器容量

                break;
            case YoloModelType::YOLO_V8:
                printf("YOLO_V8\n");
                //info:
                this->model_info.model_type = model_type;
                this->model_info.inmodel_num = 1;//初始化输入模型数量为外部参数
                this->model_info.outmodel_num = 1;//初始化输出模型数量为外部参数
                this->model_info.userptr = nullptr;
                //run_document:
                this->mod_run_doc.pthread_id.resize(this->model_info.outmodel_num);//根据外部模型有几个输出调节pthread_id容器容量

                break;

            case YoloModelType::YOLO_V26:
                printf("YOLO_V26\n");
                //info:
                this->model_info.model_type = model_type;
                this->model_info.inmodel_num = 1;//初始化输入模型数量为外部参数
                this->model_info.outmodel_num = 1;//初始化输出模型数量为外部参数
                this->model_info.userptr = nullptr;
                //run_document:
                this->mod_run_doc.pthread_id.resize(this->model_info.outmodel_num);//根据外部模型有几个输出调节pthread_id容器容量
                break;

            default:
                return -1;
        }

        //读取外部参数赋值+初始化类变量
        
        this->model_info.input_tensor.resize(this->model_info.inmodel_num);//根据外部模型有几个输入调节input_tensor容器容量
        this->model_info.output_tensor.resize(this->model_info.outmodel_num);//根据外部模型有几个输出调节output_tensor容器容量
        this->model_info.input_mem.resize(this->model_info.inmodel_num);//根据外部模型有几个输入调节input_mem容器容量
        this->model_info.output_mem.resize(this->model_info.outmodel_num);//根据外部模型有几个输出调节output_mem容器容量
        for(int i = 0 ; i < this->model_info.inmodel_num ; i++){
            this->model_info.input_tensor[i] = ex_input_attr[i];//将外部输入input_attr结构体赋值赋给类容器
            this->model_info.input_mem[i] = ex_input_mem[i];//将外部输入input_mem指针赋值赋给类容器
        }
        for(int i = 0 ; i < this->model_info.outmodel_num ; i++){
            this->model_info.output_tensor[i] = ex_output_attr[i];//将外部输出output_attr结构体赋值赋给类容器
            this->model_info.output_mem[i] = ex_output_mem[i];//将外部输出output_mem指针赋值赋给类容器
        }

        if(is_sigmoid)
        {
            this->made_dequantiza_list();
        }
        else
        {
            this->made_dequantiza_and_desigomid();
        }
        
        return 0;
    }

    int made_dequantiza_list()
    {
        int32_t zp = this->model_info.input_tensor[0].zp;
        float scale = this->model_info.input_tensor[0].scale;
        for (int i = 0; i < 256; i++) 
        {
            int8_t raw = (int8_t)(i - 128);
            float logit = (raw - zp) * scale;
            // sigmoid_table[i] = 1.0f / (1.0f + expf(-logit));
            this->mod_run_doc.dequant_table[i] = logit;
        }

        return 0;
    }
    int made_dequantiza_and_desigomid()
    {
        int32_t zp = this->model_info.input_tensor[0].zp;
        float scale = this->model_info.input_tensor[0].scale;
        for (int i = 0; i < 256; i++) 
        {
            int8_t raw = (int8_t)(i - 128);
            float logit = (raw - zp) * scale;
            this->mod_run_doc.de_quant_sigomid_table[i] = 1.0f / (1.0f + expf(-logit));
        }
        return 0;
    }


};


#endif