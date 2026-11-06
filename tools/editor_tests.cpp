#include "viewer.h"
#include "viewer_modes.h"
#include "edit_plugins.h"
#include "i18n.h"
#include "new_document_dialog.h"
#include "help.h"
#include <shlwapi.h>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::cout << "PASS " << message << std::endl;
}
namespace {
struct DialogProbe { bool capture=false, categories=false, validation=false, cancel=false; std::wstring path; } probe;
void CALLBACK ProbeDialog(HWND,UINT,UINT_PTR timer,DWORD) {
    HWND dlg=nullptr;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM out)->BOOL {
        if(GetDlgItem(window,newdoc::Width) && GetDlgItem(window,newdoc::Preset)) { *reinterpret_cast<HWND*>(out)=window; return FALSE; } return TRUE;
    },reinterpret_cast<LPARAM>(&dlg));
    if(!dlg) return;
    KillTimer(nullptr,timer);
    if(probe.cancel) { SetDlgItemTextW(dlg,newdoc::Width,L"777"); SendMessageW(dlg,WM_COMMAND,IDCANCEL,0); return; }
    RECT r;GetClientRect(dlg,&r);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=r.right;
    info.bmiHeader.biHeight=-r.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);auto old=SelectObject(dc,bitmap);
    RedrawWindow(dlg,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    if(PrintWindow(dlg,dc,PW_CLIENTONLY) && bits) {
        GdiFlush();auto* p=static_cast<uint8_t*>(bits);for(int i=0;i<r.right*r.bottom;++i)p[i*4+3]=255;
        auto formats=EnumerateSaveFormats();std::wstring error;
        for(size_t i=0;i<formats.size();++i)if(formats[i].exts[0]==L"png")
            probe.capture=SaveImage({probe.path,int(i),92},formats[i],p,r.right,r.bottom,r.right*4,false,error);
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    SendMessageW(dlg,WM_COMMAND,newdoc::Category+2,0);
    SendMessageW(dlg,WM_COMMAND,newdoc::Preset,0);
    probe.categories=GetDlgItemInt(dlg,newdoc::Width,nullptr,FALSE)==2480 && GetDlgItemInt(dlg,newdoc::Height,nullptr,FALSE)==3508;
    SendMessageW(dlg,WM_COMMAND,newdoc::Swap,0);
    probe.categories=probe.categories && GetDlgItemInt(dlg,newdoc::Width,nullptr,FALSE)==3508;
    SetDlgItemTextW(dlg,newdoc::Width,L"0");
    probe.validation=!IsWindowEnabled(GetDlgItem(dlg,IDOK));
    SendMessageW(dlg,WM_COMMAND,IDOK,0);
    probe.validation=probe.validation && IsWindow(dlg);
    SetDlgItemTextW(dlg,newdoc::Width,L"16384");SetDlgItemTextW(dlg,newdoc::Height,L"16384");
    probe.validation=probe.validation && !IsWindowEnabled(GetDlgItem(dlg,IDOK));
    SetDlgItemTextW(dlg,newdoc::Width,L"800");SetDlgItemTextW(dlg,newdoc::Height,L"600");
    probe.validation=probe.validation && IsWindowEnabled(GetDlgItem(dlg,IDOK));
    SendMessageW(dlg,WM_COMMAND,newdoc::Background+2,0);
    SendMessageW(dlg,WM_COMMAND,IDOK,0);
}
}
struct EditorIntegrationTest {
    static void Run() {
        Viewer v;
        wchar_t executable[32768]; GetModuleFileNameW(nullptr, executable, 32768);
        v.m_exeDir = ParentOf(executable); i18n::Init(v.m_exeDir, L"fr");
        v.m_hwnd = CreateWindowExW(0, L"STATIC", L"Editor offscreen tests", WS_OVERLAPPEDWINDOW,
            0, 0, 1280, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(v.m_hwnd != nullptr, "hidden window");
        Check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&v.m_factory))), "D2D factory");
        Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(v.m_dwrite.GetAddressOf()))), "DWrite");
        Check(v.CreateDeviceResources(), "graphics resources");
        v.CreateTextFormats();
        newdoc::Options newOptions;
        probe.path=v.m_exeDir+L"\\new-document-test.png";
        SetTimer(nullptr,0,150,ProbeDialog);
        Check(newdoc::Show(v.m_hwnd,newOptions), "new document dialog accepts Create");
        Check(probe.capture, "capture real new-document dialog");
        Check(probe.categories, "print templates and orientation update dimensions");
        Check(probe.validation, "invalid and oversized canvases cannot be created");
        Check(newOptions.width==800 && newOptions.height==600 && newOptions.background==2, "custom dimensions and background returned together");
        probe.cancel=true;SetTimer(nullptr,0,100,ProbeDialog);
        Check(!newdoc::Show(v.m_hwnd,newOptions) && newOptions.width==800, "cancel preserves document options");

        v.EnsureEditPlugins(); v.InitEditModes(); v.m_mode = 0;
        v.m_edit.active = v.m_edit.untitled = v.m_edit.hasAlpha = true;
        v.m_edit.w = v.m_edit.h = 64;
        Viewer::Layer background; background.bmp = v.NewCanvas(64,64);
        Check(v.DrawOn(background.bmp.Get(), [&] { v.m_dc->Clear(D2D1::ColorF(1, .25f, .1f, 1)); }), "new editable canvas");
        v.Touch(background); v.m_edit.layers.push_back(background);
        v.m_edit.color = 0x00FF00; v.m_edit.fill = true;
        v.AddObject(Viewer::Tool::Rect, D2D1::Point2F(5,5), D2D1::Point2F(20,20));
        Check(v.m_edit.layers.size() == 2 && v.CurLayer().object, "retained shape layer");
        v.PushDocUndo(); v.m_edit.moveObjectTransform = v.CurLayer().objectTransform;
        v.MoveLayerTo(25, 0);
        PixelBuffer pixels; uint32_t stride;
        Check(v.ReadBitmap(v.CurLayer().bmp.Get(), pixels, stride), "read moved object");
        Check(pixels.Data()[10*stride + 10*4+3] == 0 && pixels.Data()[10*stride + 35*4+3] == 255, "object moves without leaving paint");
        v.Undo(); Check(v.CurLayer().objectTransform._31 == 0, "undo geometry");
        v.Redo(); Check(v.CurLayer().objectTransform._31 == 25, "redo geometry");
        v.PushDocUndo(); v.CurLayer().mask = std::make_shared<std::vector<uint8_t>>(4096, uint8_t(255));
        v.m_edit.color = 0; v.m_edit.size = 20; v.m_edit.tool = Viewer::Tool::Brush;
        v.PushDocUndo(); v.PaintMask(D2D1::Point2F(35,10), D2D1::Point2F(35,10)); v.PrepareMasks();
        Check(v.ReadBitmap(v.LayerImage(1), pixels, stride) && pixels.Data()[10*stride+35*4+3] == 0, "black mask hides object");
        v.Undo(); v.PrepareMasks();
        Check(v.ReadBitmap(v.LayerImage(1), pixels, stride) && pixels.Data()[10*stride+35*4+3] == 255, "undo restores mask");
        v.ResizeCanvas(128,128); Check(v.CurLayer().mask->size() == 16384, "mask resizes with document");
        Check(v.CurLayer().objectTransform._11 == 2 && v.CurLayer().objectTransform._31 == 50, "retained geometry resizes");
        v.Undo(); Check(v.m_edit.w == 64 && v.CurLayer().mask->size() == 4096, "undo document resize");
        const std::wstring project = v.m_exeDir + L"\\editor-test.cyimg";
        std::wstring error; Check(v.WriteProject(project, error), "save native project");
        v.m_edit.dirty = false;
        Check(v.OpenProject(project), "reopen native project");
        Check(v.m_edit.layers.size() == 2 && v.CurLayer().object && v.CurLayer().mask && v.CurLayer().objectTransform._31 == 25, "objects and masks survive reopen");
        {
            const auto previous = v.CurLayer().bmp;
            std::ofstream bad(std::filesystem::path(project + L".invalid"), std::ios::binary);
            bad << "broken"; bad.close();
            Check(!v.OpenProject(project + L".invalid") && v.CurLayer().bmp == previous, "invalid project preserves current work");
            DeleteFileW((project + L".invalid").c_str());
        }
        v.CurLayer().object = false;
        v.m_edit.color = 0x0000FF; v.m_edit.size = 8; v.m_edit.tool = Viewer::Tool::Brush;
        auto beforePaint = v.CurLayer().bmp;
        Check(v.BeginStroke(), "painting starts on raster layer");
        v.StrokeTo(D2D1::Point2F(50,50), D2D1::Point2F(50,50), true);
        v.m_edit.strokeBase.Reset();
        Check(v.ReadBitmap(v.CurLayer().bmp.Get(), pixels, stride) && pixels.Data()[50*stride+50*4] > 240, "brush paints on canvas");
        v.Undo(); Check(v.CurLayer().bmp == beforePaint, "undo brush restores original bitmap");
        v.CurLayer().object = true;
        v.m_edit.fxWhole = true; v.OpenEffect(0); v.UpdateFxPreview();
        Check(v.m_edit.fxOut != nullptr, "whole-image effect preview");
        v.CloseEffect(false); Check(v.m_edit.layers.size() == 2 && v.m_edit.layers[0].visible, "cancel whole-image effect preserves layers");
        v.OpenEffect(0); v.m_edit.fxVal[0] = 25; v.UpdateFxPreview(); v.CloseEffect(true);
        Check(v.m_edit.layers.size() == 3 && !v.m_edit.layers[0].visible && v.CurLayer().visible, "whole-image result preserves hidden sources");
        v.Undo(); Check(v.m_edit.layers.size() == 2 && v.m_edit.layers[0].visible, "undo whole-image effect");
        v.m_edit.fxWhole = false; v.m_edit.layer = 0;
        for (int i = 2; i < int(v.m_modes.size()); ++i) {
            v.SwitchEditMode(i);
            if (v.m_mode == 0) continue;
            auto original = v.CurLayer().bmp;
            v.EditKey(VK_ESCAPE);
            Check(v.m_mode == 0 && v.CurLayer().bmp == original, "escape cancels plugin mode without baking");
            v.SwitchEditMode(i); v.SwitchEditMode(0);
            Check(v.CurLayer().bmp != original, "mode application commits pixels");
            v.Undo(); Check(v.CurLayer().bmp == original, "undo applied mode restores original colors");
        }
        const auto& registry = editplug::Get();
        bool testedShader = false;
        for (const auto& effect : registry.effects) if (std::string(effect.plugin->api.id) == "hlsl") {
            std::vector<uint8_t> source(8*8*4), result;
            for (size_t i=0; i<source.size(); i+=4) { source[i]=50; source[i+1]=60; source[i+2]=80; source[i+3]=128; }
            result = source;
            CyImgImage img{sizeof(CyImgImage), result.data(), source.data(), 8,8,32,{2,2,3,3},1,8,8};
            double amount = 100;
            Check(effect.api.process(nullptr,nullptr,&img,&amount) == CYIMG_OK, "HLSL compiles and executes");
            Check(result[0] == source[0] && result[(2*8+2)*4] < source[(2*8+2)*4] && result[(2*8+2)*4+3] == 128,
                "HLSL honors selection and premultiplied alpha");
            testedShader = true;
        }
        Check(testedShader, "HLSL effect discovered in gallery registry");
        v.m_edit.menu = Viewer::EditMenu::Effects;
        v.PrepareEditFrame();
        Check(!v.m_edit.galleryThumbs.empty(), "gallery renders real previews");
        auto sample=v.m_edit.gallerySample;
        auto first=v.m_edit.galleryThumbs[0];
        Check(sample && sample->GetPixelSize().width==240, "common test image is independent of document dimensions");
        PixelBuffer reference, altered; uint32_t referenceStride, alteredStride;
        Check(v.ReadBitmap(sample.Get(),reference,referenceStride), "read test scene");
        Check(v.ReadBitmap(first.Get(),altered,alteredStride) && referenceStride==alteredStride &&
            memcmp(reference.Data(),altered.Data(),referenceStride*160)!=0, "neutral-default effects have illustrative previews");
        v.m_edit.fxWhole=true; v.Touch(v.CurLayer()); v.PrepareGallery();
        Check(v.m_edit.gallerySample==sample && v.m_edit.galleryThumbs[0]==first, "target changes preserve common preview cache");
        v.m_edit.fxWhole=false;
        const auto deadline=GetTickCount64()+15000;
        while (GetTickCount64()<deadline) {
            v.OnPluginJobs(); v.PrepareGallery();
            bool ready=true;
            for (auto status:v.m_edit.galleryStatus) if (status<2) ready=false;
            if (ready) break;
            Sleep(10);
        }
        for (size_t i=0;i<registry.effects.size();++i) {
            if (registry.effects[i].disabled) continue;
            Check(v.m_edit.galleryStatus[i]==2 && v.m_edit.galleryThumbs[size_t(v.EffectCount())+i], "plugin has a rendered test-image preview");
            if (std::string(registry.effects[i].plugin->api.id)=="hlsl") {
                Check(v.ReadBitmap(v.m_edit.galleryThumbs[size_t(v.EffectCount())+i].Get(),altered,alteredStride) &&
                    memcmp(reference.Data(),altered.Data(),referenceStride*160)!=0, "HLSL preview visibly transforms the test image");
            }
        }
        v.OpenEffect(0);
        Check(v.m_edit.fxVal[0]==0 && v.m_edit.fxVal[1]==0, "illustrative gallery settings do not change effect defaults");
        v.CloseEffect(false); v.m_edit.menu=Viewer::EditMenu::Effects;

        v.m_target = v.NewCanvas(1280, 820);
        v.m_dc->SetTarget(v.m_target.Get()); v.m_dc->BeginDraw();
        v.m_dc->Clear(D2D1::ColorF(.06f, .06f, .07f, 1));
        v.DrawEditUi(1280,820);
        Check(SUCCEEDED(v.m_dc->EndDraw()), "editor and gallery draw without D2D errors");
        v.m_dc->SetTarget(nullptr);
        {
            PixelBuffer capture; uint32_t pitch;
            Check(v.ReadBitmap(v.m_target.Get(), capture, pitch), "read gallery screenshot");
            auto formats = EnumerateSaveFormats();
            for (size_t i = 0; i < formats.size(); ++i) if (formats[i].exts[0] == L"png") {
                SaveRequest request{v.m_exeDir + L"\\editor-gallery-test.png", int(i), 92};
                const auto dimensions = v.m_target->GetPixelSize();
                Check(SaveImage(request, formats[i], capture.Data(), dimensions.width, dimensions.height, pitch, false, error), "write gallery screenshot");
                break;
            }
        }
        v.m_edit.menu=Viewer::EditMenu::None; v.m_editButtons.clear(); v.m_prevEditButtons.clear();
        const Viewer::Tool panTools[]={Viewer::Tool::Move,Viewer::Tool::Brush,Viewer::Tool::Eraser,
            Viewer::Tool::Rect,Viewer::Tool::Arrow,Viewer::Tool::Text,Viewer::Tool::Fill,Viewer::Tool::Plugin};
        for(auto tool:panTools) {
            v.m_edit.tool=tool; v.ApplyFit(false);
            const auto before=v.CurLayer().bmp; const auto ver=v.CurLayer().ver;
            const auto history=v.m_edit.undo.size(), count=v.m_edit.layers.size();
            const double panX=v.m_panXT,panY=v.m_panYT;
            v.Handle(WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(400,300));
            Check(v.m_rightPanning && v.m_dragging && !v.m_edit.drawing, "right button grabs view regardless of active tool");
            v.Handle(WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(445,325));
            Check(v.m_panXT==panX+45 && v.m_panYT==panY+25, "right drag moves small image view by pointer distance");
            v.Handle(WM_LBUTTONDOWN,MK_RBUTTON|MK_LBUTTON,MAKELPARAM(445,325));
            v.Handle(WM_LBUTTONUP,MK_RBUTTON,MAKELPARAM(445,325));
            Check(v.m_rightPanning && !v.m_edit.drawing, "left button cannot interrupt a right pan or start editing");
            v.Handle(WM_RBUTTONUP,0,MAKELPARAM(445,325));
            Check(!v.m_dragging && !v.m_rightPanning && GetCapture()!=v.m_hwnd &&
                v.CurLayer().bmp==before && v.CurLayer().ver==ver && v.m_edit.undo.size()==history &&
                v.m_edit.layers.size()==count && !v.m_edit.textActive, "releasing right pan preserves pixels objects and history");
        }
        v.Handle(WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(400,300));
        v.Handle(WM_CAPTURECHANGED,0,0);
        Check(!v.m_dragging && !v.m_rightPanning, "lost mouse capture releases right pan"); ReleaseCapture();
        for(int mode=1;mode<int(v.m_modes.size());++mode) {
            v.SwitchEditMode(mode); v.ApplyFit(false);
            v.Handle(WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(400,300));
            Check(v.m_rightPanning && !v.m_edit.drawing, "right drag also grabs view in other editing modes");
            v.Handle(WM_RBUTTONUP,0,MAKELPARAM(400,300));
            v.SwitchEditMode(0);
        }
        v.SwitchEditMode(0);
        Check(cyhelp::Available(v.m_exeDir), "offline manual is beside the built application");
        const auto special=cyhelp::Url(L"D:\\Apps\\Photos %20 & #1",L"fr",{"layers","masks","mask-demo"});
        const auto base=special.substr(0,special.find(L'#'));
        wchar_t restored[32768]{};DWORD restoredSize=32768;
        const auto restoredHr=PathCreateFromUrlW(base.c_str(),restored,&restoredSize,0);
        Check(SUCCEEDED(restoredHr) &&
            std::wstring(restored)==L"D:\\Apps\\Photos %20 & #1\\Documentation\\Documentation.html", "file URL round-trip preserves spaces percent ampersand and hash");
        Check(special.find(L"#/fr/layers/masks?media=mask-demo")!=std::wstring::npos, "CyDoc route keeps section and media inside fragment");
        Check(cyhelp::Url(v.m_exeDir,L"unknown",{"overview"}).find(L"#/en/overview")!=std::wstring::npos, "unknown application locale falls back to English help");
        Check(cyhelp::Url(v.m_exeDir,L"de",{"layers","masks"}).find(L"#/en/layers/masks")!=std::wstring::npos, "interface locales without a complete manual open English contextual help");
        std::wstring helpLaunch;
        v.m_helpLauncher=[&](const std::wstring& url){helpLaunch=url;};
        auto f1=[&](const wchar_t* expected) {
            helpLaunch.clear();v.Handle(WM_KEYDOWN,VK_F1,0);
            Check(helpLaunch.find(expected)!=std::wstring::npos,"F1 selects the contextual manual page");
        };
        v.m_edit.menu=Viewer::EditMenu::None;v.m_edit.fx=-1;v.m_edit.tool=Viewer::Tool::Brush;
        const auto oldHistory=v.m_edit.undo.size();const auto oldBitmap=v.CurLayer().bmp;
        f1(L"/edit/paint");
        v.m_edit.tool=Viewer::Tool::Gradient;f1(L"/edit/gradient");
        v.m_edit.tool=Viewer::Tool::Text;v.m_edit.textActive=true;v.m_edit.text=L"Unfinished text";
        f1(L"/edit/objects");Check(v.m_edit.textActive && v.m_edit.text==L"Unfinished text","F1 does not commit or discard text");v.m_edit.textActive=false;v.m_edit.text.clear();
        v.m_edit.editMask=true;f1(L"/layers/masks");v.m_edit.editMask=false;
        v.m_edit.menu=Viewer::EditMenu::Effects;f1(L"/effects/gallery");v.m_edit.menu=Viewer::EditMenu::None;
        v.m_edit.fx=0;f1(L"/effects/apply");Check(v.m_edit.fx==0,"help leaves the live effect open");v.m_edit.fx=-1;
        v.EditCommand(Viewer::EditCmd::Help,0);Check(helpLaunch.find(L"/edit/objects")!=std::wstring::npos,"editor help command uses the contextual route");
        for(int mode=1;mode<int(v.m_modes.size());++mode){
            const auto* plugin=mode>=2?editplug::Get().modes[size_t(mode-2)].plugin:nullptr;
            const bool blackWhite=plugin && std::string(plugin->api.id)=="bw_mode";
            v.SwitchEditMode(mode);f1(mode==1?L"/develop/adjust":blackWhite?L"/black-white/cancel":L"/plugins/modes");
            v.Mode().Close();v.SwitchEditMode(0);
        }
        v.m_edit.active=false;f1(L"/viewer/navigation");v.m_showSettings=true;f1(L"/settings/settings");v.m_showSettings=false;v.m_edit.active=true;
        Check(v.m_edit.undo.size()==oldHistory && v.CurLayer().bmp==oldBitmap,"help routing preserves document pixels and undo history");
        v.m_helpLauncher={};
        v.m_edit.active = false; v.m_showStrip = true; v.m_files.resize(2);
        const auto area = v.ViewArea(); RECT client; GetClientRect(v.m_hwnd,&client);
        Check(area.top >= 52*v.m_scale && area.bottom <= client.bottom - 80*v.m_scale, "fit reserves toolbar and filmstrip");
        v.m_edit.active = true; v.m_edit.dirty = false;
        v.EndEdit(false); DestroyWindow(v.m_hwnd); v.m_hwnd = nullptr;
        DeleteFileW(project.c_str());
    }
};
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try { EditorIntegrationTest::Run(); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << std::endl; return 1; }
    CoUninitialize(); return 0;
}
