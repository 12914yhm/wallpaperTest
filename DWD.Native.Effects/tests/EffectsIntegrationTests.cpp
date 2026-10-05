#include <windows.h>
#include "WallpaperEffects.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Pump(DWORD milliseconds){const ULONGLONG end=GetTickCount64()+milliseconds;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}while(GetTickCount64()<end);}
std::wstring Quote(const std::wstring& s){return L"\""+s+L"\"";}
void Run(const std::wstring& exe,const std::wstring& path,HWND window,bool image){
 const auto name=L"Local\\DWDIntegration_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
 HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,(name+L"S").c_str()),ready=CreateEventW(nullptr,TRUE,FALSE,(name+L"R").c_str()),pause=CreateEventW(nullptr,TRUE,FALSE,(name+L"P").c_str());
 EffectChannel channel;Check(channel.Open(name,true),"channel");WallpaperEffects effect;effect.kind=1;channel.Write(effect);
 std::wstring command=Quote(exe)+L" --player --video "+Quote(path)+L" --stop-event "+Quote(name+L"S")+L" --ready-event "+Quote(name+L"R")+
  L" --pause-event "+Quote(name+L"P")+L" --effects-channel "+Quote(name)+L" --parent-pid "+std::to_wstring(GetCurrentProcessId())+
  L" --preview-window "+std::to_wstring(reinterpret_cast<UINT_PTR>(window));
 STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
 Check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch");CloseHandle(process.hThread);
 try{
  const auto deadline=GetTickCount64()+15000;
  while(WaitForSingleObject(ready,0)!=WAIT_OBJECT_0&&WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0&&GetTickCount64()<deadline)Pump(30);
  Check(WaitForSingleObject(ready,0)==WAIT_OBJECT_0,"first frame");
  auto* data=channel.Data();for(int kind=1;kind<=3;++kind){effect.kind=kind;effect.brightness=80;Check(channel.Write(effect),"write effects");
   const LONG before=data->frames;Pump(350);Check(data->frames>before&&data->error==0,"particle frames");}
  if(!image){Check(data->durationMs>0,"duration");InterlockedExchange(&data->seekMs,data->durationMs/2);Pump(500);Check(data->positionMs>data->durationMs/3,"seek");}
  SetEvent(pause);Pump(1200);const LONG paused=data->frames;Pump(350);Check(data->frames==paused,"pause");ResetEvent(pause);Pump(1200);Check(data->frames>paused,"resume");
  channel.Write({});Pump(500);const auto noEffects=data->frames;Pump(500);
  if(image)Check(data->frames==noEffects,"static image stops rendering");
  effect.kind=3;channel.Write(effect);Pump(500);Check(data->frames>noEffects,"re-enable");
  SetEvent(stop);const auto stopDeadline=GetTickCount64()+5000;
  while(WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0&&GetTickCount64()<stopDeadline)Pump(20);
  Check(WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0,"graceful stop");
  DWORD code{};GetExitCodeProcess(process.hProcess,&code);Check(code==0,"exit success");
  std::cout<<"PASS native "<<(image?"image":"video")<<" effects/three particles/disable/pause/stop\n";
 }catch(...){SetEvent(stop);if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,9);
  CloseHandle(process.hProcess);CloseHandle(stop);CloseHandle(ready);CloseHandle(pause);throw;}
 CloseHandle(process.hProcess);CloseHandle(stop);CloseHandle(ready);CloseHandle(pause);
}
int wmain(int argc,wchar_t** argv){if(argc<3)return 2;
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DWDEffectIntegrationHost";RegisterClassW(&wc);
 HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"DWD effects test",WS_POPUP|WS_VISIBLE,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
 const auto image=std::filesystem::current_path()/(L"effect-fixture-"+std::to_wstring(GetCurrentProcessId())+L".bmp");
 try{
  BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};ih.biSize=sizeof(ih);ih.biWidth=640;ih.biHeight=360;ih.biPlanes=1;ih.biBitCount=32;
  fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+640*360*4;
  std::vector<unsigned> pixels(640*360);for(int y=0;y<360;++y)for(int x=0;x<640;++x)pixels[y*640+x]=0xff000000|(x*255/640)<<16|(y*255/360)<<8|80;
  {std::ofstream file(image,std::ios::binary);file.write(reinterpret_cast<char*>(&fh),sizeof(fh));file.write(reinterpret_cast<char*>(&ih),sizeof(ih));file.write(reinterpret_cast<char*>(pixels.data()),pixels.size()*4);}
  Run(argv[1],argv[2],window,false);Run(argv[1],image.wstring(),window,true);
  DestroyWindow(window);std::filesystem::remove(image);return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";DestroyWindow(window);std::filesystem::remove(image);return 1;}
}
