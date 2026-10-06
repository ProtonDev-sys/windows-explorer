#pragma once
// Shared independent native extraction and raw four-byte raster oracle.
#include <windows.h>
#include <uiribbon.h>
#include <shlobj.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <string>

namespace native_icon_reference {
inline void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
inline void succeeded(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(static_cast<ULONG>(result)));}
struct Icon {
    HICON handle=nullptr;
    ~Icon(){if(handle)DestroyIcon(handle);}
};
struct IconRaster {
    HBITMAP bitmap=nullptr;
    HDC dc=nullptr;
    HGDIOBJ previous=nullptr;
    ~IconRaster(){
        if(dc){if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);DeleteDC(dc);}
        if(bitmap)DeleteObject(bitmap);
    }
};
inline std::vector<BYTE> bitmapPixels(HBITMAP bitmap,UINT pixels) {
    require(GdiFlush()!=FALSE,"Flush native icon raster before reading pixels");
    DIBSECTION section{};
    require(bitmap&&GetObjectW(bitmap,sizeof(section),&section)==sizeof(section),"Read actual native icon DIB section");
    const auto& actual=section.dsBm;
    require(actual.bmWidth==static_cast<LONG>(pixels)&&actual.bmHeight==static_cast<LONG>(pixels)&&actual.bmBitsPixel==32&&actual.bmPlanes==1&&
        actual.bmWidthBytes==static_cast<LONG>(pixels*4)&&actual.bmBits&&section.dsBmih.biBitCount==32&&
        section.dsBmih.biCompression==BI_RGB&&section.dsBmih.biPlanes==1&&
        (section.dsBmih.biHeight==static_cast<LONG>(pixels)||section.dsBmih.biHeight==-static_cast<LONG>(pixels)),
        "Native icon DIB dimensions, stride, orientation, or format changed");
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
    info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    IconRaster readback;readback.dc=CreateCompatibleDC(nullptr);
    require(readback.dc!=nullptr,"Create native icon readback DC");
    std::vector<BYTE> bytes(static_cast<size_t>(pixels)*pixels*4);
    require(GetDIBits(readback.dc,bitmap,0,pixels,bytes.data(),&info,DIB_RGB_COLORS)==static_cast<int>(pixels),
        "Read every native icon bitmap scanline");
    const auto* stored=static_cast<const BYTE*>(actual.bmBits);
    std::vector<BYTE> raw(bytes.size());
    // Both the production ownership-transferred bitmap and this independent
    // raster are created with negative height. GetObject reports positive
    // height on the observed native implementation, so do not infer storage
    // orientation from that returned sign. Require its actual raw RGB rows to
    // equal the independently requested top-down GetDIBits rows before using
    // all four stored bytes, including alpha, for the exact comparison.
    std::memcpy(raw.data(),stored,raw.size());
    for(size_t pixel=0;pixel<raw.size();pixel+=4)
        require(std::memcmp(bytes.data()+pixel,raw.data()+pixel,3)==0,"Native top-down GetDIBits RGB readback differs from stored pixels");
    return raw;
}
inline std::vector<BYTE> iconPixels(HICON icon,UINT pixels) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
    info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    IconRaster raster;void* bits=nullptr;
    raster.bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    require(raster.bitmap&&bits,"Create independent native icon raster");
    raster.dc=CreateCompatibleDC(nullptr);require(raster.dc!=nullptr,"Create independent native icon drawing DC");
    raster.previous=SelectObject(raster.dc,raster.bitmap);
    require(raster.previous&&raster.previous!=HGDI_ERROR,"Select independent native icon raster");
    std::memset(bits,0,static_cast<size_t>(pixels)*pixels*4);
    require(DrawIconEx(raster.dc,0,0,icon,static_cast<int>(pixels),static_cast<int>(pixels),0,nullptr,DI_NORMAL)!=FALSE,
        "Draw independently extracted native icon");
    const auto restored=SelectObject(raster.dc,raster.previous);
    require(restored&&restored!=HGDI_ERROR,"Deselect independent native icon raster before reading pixels");
    raster.previous=nullptr;
    return bitmapPixels(raster.bitmap,pixels);
}
struct NativeIconReference {
    HRESULT extraction=S_OK;
    bool iconPresent=false;
    std::vector<BYTE> pixels;
    HRESULT imageStatus()const noexcept {
        return FAILED(extraction)?extraction:iconPresent?S_OK:E_FAIL;
    }
};
inline NativeIconReference singleIconReference(const std::wstring& path,int resource,UINT pixels) {
    Icon icon;
    const auto packedSize=MAKELONG(pixels,0);
    NativeIconReference reference;
    reference.extraction=SHDefExtractIconW(path.c_str(),resource,0,&icon.handle,nullptr,packedSize);
    reference.iconPresent=icon.handle!=nullptr;
    if(SUCCEEDED(reference.imageStatus()))reference.pixels=iconPixels(icon.handle,pixels);
    const auto module=std::filesystem::path(path).filename().string();
    std::cout<<"Native single icon contract module="<<module<<" resourceIndex="<<resource
        <<" requestedPixels="<<pixels<<" flags=0 packedSize="<<packedSize
        <<" HRESULT="<<static_cast<ULONG>(reference.extraction)<<" iconPresent="<<reference.iconPresent
        <<" imageHRESULT="<<static_cast<ULONG>(reference.imageStatus())<<'\n';
    return reference;
}
inline std::vector<BYTE> ribbonImagePixels(IUIImage* image,UINT pixels) {
    require(image!=nullptr,"Actual Ribbon cached native image is missing");
    HBITMAP bitmap=nullptr;succeeded(image->GetBitmap(&bitmap),"Get actual Ribbon cached bitmap");
    return bitmapPixels(bitmap,pixels);
}
inline size_t requireImageContract(HRESULT status,IUIImage* image,const NativeIconReference& reference,UINT pixels) {
    require(status==reference.imageStatus(),"Actual Ribbon image changed the original single-size normalized HRESULT");
    if(FAILED(reference.imageStatus())) {
        require(image==nullptr,"Failed native Ribbon image returned an output");
        return 0;
    }
    const auto bytes=ribbonImagePixels(image,pixels);
    require(bytes==reference.pixels,"Actual native Ribbon image differs from original single-size RGBA pixels");
    return bytes.size();
}
} // namespace native_icon_reference
