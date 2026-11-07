#include "thumbnail_image.hpp"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>

namespace qs {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint8_t signature[]={137,80,78,71,13,10,26,10};
struct Apartment {
    const HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~Apartment(){if(SUCCEEDED(status)) CoUninitialize();}
    bool ready() const {return SUCCEEDED(status)||status==RPC_E_CHANGED_MODE;}
};
ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> out;
    CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&out));
    return out;
}
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}
bool image_header(const uint8_t *p,size_t n) {
    return n>=33 && n<=max_preview_png_bytes && std::memcmp(p,signature,8)==0 &&
        be32(p+8)==13 && std::memcmp(p+12,"IHDR",4)==0 && be32(p+16)>0 && be32(p+20)>0 &&
        be32(p+16)<=2048 && be32(p+20)<=2048 && uint64_t(be32(p+16))*be32(p+20)<=1024*1024;
}
bool part_length(const uint8_t *p,size_t &size) {
    const char *first=reinterpret_cast<const char*>(p);
    auto result=std::from_chars(first,first+8,size);
    return result.ec==std::errc{} && (result.ptr==first+8||*result.ptr=='\0');
}
}
Bytes native_thumbnail(const Bytes &checkpoint) {
    if(checkpoint.size()<24) return {};
    size_t meta=0, media=0, state=0;
    if(!part_length(checkpoint.data(),meta) || !part_length(checkpoint.data()+8,media) ||
       !part_length(checkpoint.data()+16,state) || 24ull+meta+media+state!=checkpoint.size() ||
       !image_header(checkpoint.data()+24+meta,media)) return {};
    const auto begin=checkpoint.begin()+24+meta;
    return Bytes(begin,begin+media);
}
Bytes encode_thumbnail(const ThumbnailPixels &image) {
    if(!image.width||!image.height||image.width>preview_width||image.height>preview_height ||
       image.rgba.size()!=size_t(image.width)*image.height*4) return {};
    Apartment apartment;if(!apartment.ready()) return {};
    auto wic=factory();if(!wic) return {};
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> bitmap;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)) ||
       FAILED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) ||
       FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) ||
       FAILED(encoder->CreateNewFrame(&frame,nullptr)) || FAILED(frame->Initialize(nullptr)) ||
       FAILED(frame->SetSize(image.width,image.height))) return {};
    auto format=GUID_WICPixelFormat32bppRGBA;
    if(FAILED(frame->SetPixelFormat(&format)) ||
       FAILED(wic->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppRGBA,
            image.width*4,static_cast<UINT>(image.rgba.size()),const_cast<BYTE*>(image.rgba.data()),&bitmap)) ||
       FAILED(frame->WriteSource(bitmap.Get(),nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return {};
    STATSTG stat{};
    if(FAILED(stream->Stat(&stat,STATFLAG_NONAME)) || stat.cbSize.QuadPart>max_preview_png_bytes || !stat.cbSize.QuadPart) return {};
    Bytes out(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER start{};ULONG count=0;
    if(FAILED(stream->Seek(start,STREAM_SEEK_SET,nullptr)) ||
       FAILED(stream->Read(out.data(),static_cast<ULONG>(out.size()),&count)) || count!=out.size()) return {};
    return out;
}
ThumbnailPixels decode_thumbnail(const Bytes &png) {
    if(!image_header(png.data(),png.size())) return {};
    Apartment apartment;if(!apartment.ready()) return {};
    auto wic=factory();if(!wic) return {};
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if(FAILED(wic->CreateStream(&stream)) ||
       FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),static_cast<DWORD>(png.size()))) ||
       FAILED(wic->CreateDecoder(GUID_ContainerFormatPng,nullptr,&decoder)) ||
       FAILED(decoder->Initialize(stream.Get(),WICDecodeMetadataCacheOnDemand)) ||
       FAILED(decoder->GetFrame(0,&frame)) || FAILED(wic->CreateFormatConverter(&converter))) return {};
    UINT w=0,h=0;if(FAILED(frame->GetSize(&w,&h)) || w!=be32(png.data()+16) || h!=be32(png.data()+20)) return {};
    IWICBitmapSource *source=frame.Get();
    ComPtr<IWICBitmapScaler> scaler;
    const double scale=std::min({1.,double(preview_width)/w,double(preview_height)/h});
    if(scale<1.) {
        w=std::max(1u,static_cast<unsigned>(std::lround(w*scale)));
        h=std::max(1u,static_cast<unsigned>(std::lround(h*scale)));
        if(FAILED(wic->CreateBitmapScaler(&scaler)) ||
           FAILED(scaler->Initialize(source,w,h,WICBitmapInterpolationModeFant))) return {};
        source=scaler.Get();
    }
    if(FAILED(converter->Initialize(source,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,
                                    nullptr,0,WICBitmapPaletteTypeCustom))) return {};
    ThumbnailPixels out{w,h,Bytes(size_t(w)*h*4)};
    if(FAILED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(out.rgba.size()),out.rgba.data()))) return {};
    return out;
}
}
