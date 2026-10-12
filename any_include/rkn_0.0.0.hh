#ifndef RKN_0_0_0_HH
#define RKN_0_0_0_HH
enum io_type{
    PSQ_RKNN_INPUT = 0,
    PSQ_RKNN_OUTPUT = 1,
};

#include <string>
#include <vector>
#include <rknpu2/rknn_api.h>
#include <unistd.h>
#include <string.h>


struct model_info{
    std::string model_path;
    size_t input_size;
    size_t output_size;
    std::vector<rknn_tensor_attr> input_attr;
    std::vector<rknn_tensor_attr> output_attr;
    std::vector<rknn_tensor_mem*> output_mem;
    std::vector<rknn_tensor_mem*> input_mem;
    rknn_context ctx;
};

class PSQ_rknn{
public:

    bool debug = false;

    PSQ_rknn(const char *model_path , size_t input_size, size_t output_size)
    {
        this->model_info_.model_path = model_path;
        this->model_info_.input_size = input_size;
        this->model_info_.output_size = output_size;

        rknn_init(&this->model_info_.ctx , (void*)model_path,0,0,NULL);
        this->model_info_.input_attr.resize(input_size);
        this->model_info_.input_mem.resize(input_size);
        this->model_info_.output_attr.resize(output_size);
        this->model_info_.output_mem.resize(output_size);
        for (size_t i = 0; i < input_size; i++)
        {
            memset(&this->model_info_.input_attr[i], 0, sizeof(rknn_tensor_attr));
            this->model_info_.input_attr[i].index = i; 
            rknn_query(this->model_info_.ctx, RKNN_QUERY_INPUT_ATTR, &this->model_info_.input_attr[i], sizeof(rknn_tensor_attr));

            this->model_info_.input_mem[i] = rknn_create_mem(this->model_info_.ctx , this->model_info_.input_attr[i].size);
            rknn_set_io_mem(this->model_info_.ctx,this->model_info_.input_mem[i],&this->model_info_.input_attr[i]);
        
        }
        for (size_t i = 0; i < output_size; i++)
        {
            memset(&this->model_info_.output_attr[i], 0, sizeof(rknn_tensor_attr)); 
            this->model_info_.output_attr[i].index = i; 
            rknn_query(this->model_info_.ctx, RKNN_QUERY_OUTPUT_ATTR, &this->model_info_.output_attr[i], sizeof(rknn_tensor_attr));
        
            this->model_info_.output_mem[i] = rknn_create_mem(this->model_info_.ctx , this->model_info_.output_attr[i].size);
            rknn_set_io_mem(this->model_info_.ctx,this->model_info_.output_mem[i],&this->model_info_.output_attr[i]);
        }

    }

    rknn_tensor_mem** get_mem(io_type type)
    {
        if(type == PSQ_RKNN_INPUT)
        {
            return this->model_info_.input_mem.data();
        }
        else if(type == PSQ_RKNN_OUTPUT)
        {
            return this->model_info_.output_mem.data();
        }
        return nullptr;
    }

    rknn_tensor_attr* get_attr(io_type type)
    {
        if(type == PSQ_RKNN_INPUT)
        {
            return this->model_info_.input_attr.data();
        }
        else if(type == PSQ_RKNN_OUTPUT)
        {
            return this->model_info_.output_attr.data();
        }
        return nullptr;
    }

    void print_info()
    {
        printf("输入数量: %d\n", this->model_info_.input_size);
        printf("输出数量: %d\n", this->model_info_.output_size);
        for(int i = 0; i < this->model_info_.input_size; i++)
        {
            printf("==========【attr】输入 %d==========\n", i);
            printf("Tensor维度个数:%d \n" , this->model_info_.input_attr[i].n_dims);
            printf("Tensor形状:(%d , %d , %d , %d) \n" , this->model_info_.input_attr[i].dims[0] , this->model_info_.input_attr[i].dims[1] , this->model_info_.input_attr[i].dims[2] ,  this->model_info_.input_attr[i].dims[3]);
            printf("Tensor数据元素个数:%d \n" , this->model_info_.input_attr[i].n_elems);
            printf("Tensor数据所占内存大小:%d\n" , this->model_info_.input_attr[i].size);
            printf("Tensor维度的格式:%d\n" , this->model_info_.input_attr[i].fmt);
            printf("Tensor数据类型:%d\n" , this->model_info_.input_attr[i].type);
        }
        for(int i = 0; i < this->model_info_.output_size; i++)
        {
            printf("==========【attr】输出 %d==========\n", i);
            printf("Tensor维度个数:%d \n" , this->model_info_.output_attr[i].n_dims);
            printf("Tensor形状:(%d , %d , %d , %d) \n" , this->model_info_.output_attr[i].dims[0] , this->model_info_.output_attr[i].dims[1] , this->model_info_.output_attr[i].dims[2] ,  this->model_info_.output_attr[i].dims[3]);
            printf("Tensor数据元素个数:%d \n" , this->model_info_.output_attr[i].n_elems);
            printf("Tensor数据所占内存大小:%d\n" , this->model_info_.output_attr[i].size);
            printf("Tensor维度的格式:%d\n" , this->model_info_.output_attr[i].fmt);
            printf("Tensor数据类型:%d\n" , this->model_info_.output_attr[i].type);
        }
    }

    int run_single(void *input)//先只来单输入——后续在加
    {
        memcpy(this->model_info_.input_mem[0]->virt_addr, input, this->model_info_.input_attr[0].size);
        rknn_mem_sync(this->model_info_.ctx, this->model_info_.input_mem[0], RKNN_MEMORY_SYNC_TO_DEVICE);
    
        int ret = rknn_run(this->model_info_.ctx, NULL);
        if(ret != RKNN_SUCC)
        {
            printf("--【rknn】PSQ_rknn: run_single： %d 失败\n", ret);
            return ret;
        }

        for(int i = 0 ; i < this->model_info_.output_size; i++)
        {
            rknn_mem_sync(this->model_info_.ctx, this->model_info_.output_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
        }
        return 0;
    }


    ~PSQ_rknn()
    {
        rknn_destroy(this->model_info_.ctx);
        for(int i = 0; i < this->model_info_.input_size; i++)
        {
            rknn_destroy_mem(this->model_info_.ctx, this->model_info_.input_mem[i]);
        }
        for(int i = 0; i < this->model_info_.output_size; i++)
        {
            rknn_destroy_mem(this->model_info_.ctx, this->model_info_.output_mem[i]);
        }
    }


private:

    model_info model_info_;




};


#endif