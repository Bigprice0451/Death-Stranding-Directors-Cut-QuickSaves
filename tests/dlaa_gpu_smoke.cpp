// Optional hardware check. See DLAA.md for build and run instructions. The two
// arguments select the signed DLSS runtime directory and a writable log folder.
#include "dlaa_ngx.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "nvsdk_ngx_helpers.h"
#include <cstdio>
#include <stdexcept>
#include <cwchar>
#include <filesystem>
using namespace qs::dlaa_detail;
using Microsoft::WRL::ComPtr;
namespace {
void check_hr(HRESULT hr, const char *where) { if (FAILED(hr)) { std::printf("%s HRESULT %08lx\n",where,hr); throw std::runtime_error(where); } }
void check_ngx(U32 result, const char *where) { if (!success(result)) { std::printf("%s NGX %08x\n",where,result); throw std::runtime_error(where); } }
NVSDK_NGX_Parameter *param(void *p) { return static_cast<NVSDK_NGX_Parameter *>(p); }
void set_i(void *p, const char *k, int v) { param(p)->Set(k,v); }
void set_u(void *p, const char *k, U32 v) { param(p)->Set(k,v); }
U32 get_i(void *p, const char *k, int *v) { return param(p)->Get(k,v); }
U32 get_u(void *p, const char *k, U32 *v) { return param(p)->Get(k,v); }
bool query(void *p, U32 w, U32 h, int mode, bool, U32 *iw, U32 *ih, float *sharp) {
    U32 max_w{},max_h{},min_w{},min_h{};
    return success(NGX_DLSS_GET_OPTIMAL_SETTINGS(param(p),w,h,static_cast<NVSDK_NGX_PerfQuality_Value>(mode),
                   iw,ih,&max_w,&max_h,&min_w,&min_h,sharp));
}
U32 driver_create(void *cmd, U32 type, void *p, void **handle) {
    int mode{}; U32 hint{};
    check_ngx(get_i(p,quality_key,&mode), "mode readback");
    check_ngx(get_u(p,preset_key,&hint), "hint readback");
    std::printf("Request sent to NGX: mode %d, DLAA preset %u\n",mode,hint);
    NVSDK_NGX_Handle *result_handle{};
    const U32 r=NVSDK_NGX_D3D12_CreateFeature(static_cast<ID3D12GraphicsCommandList *>(cmd),
        static_cast<NVSDK_NGX_Feature>(type),param(p),&result_handle);
    *handle=result_handle;
    return r;
}
ComPtr<ID3D12Resource> texture(ID3D12Device *device, Size s, DXGI_FORMAT format, bool writable) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type=D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=s.w; desc.Height=s.h;
    desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Format=format;
    if(writable) desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    check_hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,IID_PPV_ARGS(&resource)), "test texture");
    return resource;
}
}
int wmain(int argc, wchar_t **argv) {
    if(argc!=3 && argc!=5) return 2;
    try {
        ComPtr<IDXGIFactory6> factory; check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI");
        ComPtr<ID3D12Device> device;
        for(UINT i=0; !device; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            check_hr(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)), "NVIDIA adapter");
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if(d.VendorId==0x10de && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
                std::printf("Adapter: %ls\n",d.Description);
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{}; ComPtr<ID3D12CommandQueue> queue;
        check_hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)),"command queue");
        ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> cmd;
        check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        check_hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)),"command list");
        const wchar_t *plugins[]={argv[1]}; NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo={plugins,1};
        check_ngx(NVSDK_NGX_D3D12_Init_with_ProjectID("7ff452a7-2fb7-4ba6-ada9-1dcbd29a61ff",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                    "1.0",argv[2],device.Get(),&common),"NGX init");
        NVSDK_NGX_Parameter *p{}; check_ngx(NVSDK_NGX_D3D12_GetCapabilityParameters(&p),"NGX capabilities");
        const Size output = argc == 5 ? Size{U32(std::wcstoul(argv[3],nullptr,10)), U32(std::wcstoul(argv[4],nullptr,10))} : Size{1280,720};
        if(!output.w || !output.h || output.w>8192 || output.h>8192) throw std::runtime_error("invalid test resolution");
        for(bool on : {true,false}) {
            Size input; bool native{}; float sharpness{};
            if(!optimal(query,p,output,quality,false,&input.w,&input.h,&sharpness,on,native) || native != on)
                throw std::runtime_error("driver returned unexpected sizing");
            set_i(p,quality_key,quality); set_u(p,"Width",input.w); set_u(p,"Height",input.h);
            set_u(p,"OutWidth",output.w); set_u(p,"OutHeight",output.h);
            set_i(p,"DLSS.Feature.Create.Flags",0x2b); set_u(p,"CreationNodeMask",1); set_u(p,"VisibilityNodeMask",1);
            void *handle{}; Created feature;
            check_ngx(create({set_i,set_u,get_i,get_u},driver_create,cmd.Get(),1,p,&handle,on,output,feature),"NGX create");
            if(!handle || feature.preset_requested != on) throw std::runtime_error("unexpected preset request");
            wchar_t module_path[1024]{}; auto module=GetModuleHandleW(L"nvngx_dlss.dll");
            if(!module || !GetModuleFileNameW(module,module_path,1024) ||
                !std::filesystem::equivalent(module_path,std::filesystem::path(argv[1])/L"nvngx_dlss.dll"))
                throw std::runtime_error("NGX did not load the specified test runtime");
            std::printf("Loaded DLSS library: %ls\n",module_path);
            auto col=texture(device.Get(),input,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
            auto out=texture(device.Get(),output,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
            auto depth=texture(device.Get(),input,DXGI_FORMAT_R32_FLOAT,false);
            auto mv=texture(device.Get(),input,DXGI_FORMAT_R16G16_FLOAT,false);
            NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
            eval.Feature.pInColor=col.Get(); eval.Feature.pInOutput=out.Get(); eval.pInDepth=depth.Get(); eval.pInMotionVectors=mv.Get();
            eval.InReset=1; eval.InMVScaleX=eval.InMVScaleY=1; eval.InRenderSubrectDimensions.Width=input.w; eval.InRenderSubrectDimensions.Height=input.h;
            check_ngx(NGX_D3D12_EVALUATE_DLSS_EXT(cmd.Get(),static_cast<NVSDK_NGX_Handle *>(handle),p,&eval),"NGX evaluation");
            check_hr(cmd->Close(),"close commands"); ID3D12CommandList *lists[]={cmd.Get()}; queue->ExecuteCommandLists(1,lists);
            ComPtr<ID3D12Fence> fence; check_hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
            check_hr(queue->Signal(fence.Get(),1),"signal");
            check_hr(fence->SetEventOnCompletion(1,nullptr),"wait for GPU"); check_hr(device->GetDeviceRemovedReason(),"GPU completion");
            const auto in_desc=col->GetDesc(),out_desc=out->GetDesc();
            if(native_frame(input,{U32(in_desc.Width),in_desc.Height},{U32(out_desc.Width),out_desc.Height},output) != on)
                throw std::runtime_error("size mismatch");
            std::printf("Evaluated %s input %ux%u -> output %ux%u. GPU fence completed.\n",on?"DLAA":"Quality",input.w,input.h,output.w,output.h);
            check_ngx(NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle *>(handle)),"release feature");
            if(on) {
                check_hr(allocator->Reset(),"reset allocator");
                check_hr(cmd->Reset(allocator.Get(),nullptr),"reset command list");
            }
        }
        check_ngx(NVSDK_NGX_D3D12_DestroyParameters(p),"release parameters");
        check_ngx(NVSDK_NGX_D3D12_Shutdown1(device.Get()),"shutdown");
        return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"Smoke failed: %s\n",e.what()); return 1; }
}
