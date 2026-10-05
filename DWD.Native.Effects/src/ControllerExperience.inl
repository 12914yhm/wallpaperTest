// 体验扩展集中在此文件；复用 Controller 的 UI 线程和便携写入队列，不新增常驻服务。
namespace Experience {
constexpr int Tab=2000,Kind=2010,Slider=2020,Compare=2030,Reset=2031,Apply=2032;
constexpr int Group=2040,List=2041,Enabled=2042,Interval=2043,Random=2044,Schedule=2045,Next=2046;
constexpr int AllScreens=2050,Screen=2060,Accent=2070,Ball=2071,Locate=2072,Open=2073,PreviewPause=2074;
constexpr int ClosePanel=2080;
constexpr int SliderLabel=2100;
HWND list{},group{};bool rebuilding{};std::map<int,HWND> controls;std::vector<RECT> screenRects;
const wchar_t* names[]{L"粒子密度",L"运动速度",L"透明度",L"亮度",L"饱和度",L"冷暖"};
const int minimum[]{0,1,0,30,0,-50},maximum[]{100,100,100,160,180,50};
HWND Control(HWND owner,int id,const wchar_t* cls,const wchar_t* text,DWORD style){
 const bool button=wcscmp(cls,L"BUTTON")==0 && style==BS_PUSHBUTTON;
 if(wcscmp(cls,L"COMBOBOX")==0){HWND h=MakeThemeCombo(owner,id,g_smallFont);controls[id]=h;return h;}
 if(button)style=BS_OWNERDRAW;
 HWND h=CreateWindowExW(0,cls,text,WS_CHILD|WS_TABSTOP|WS_CLIPSIBLINGS|style,0,0,0,0,owner,
  reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
 SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(g_smallFont),TRUE);
 if(wcscmp(cls,TRACKBAR_CLASSW)==0)SetWindowSubclass(h,SliderPaintProc,1,0);
 if(button)SetWindowSubclass(h,ButtonSubclass,1,0);controls[id]=h;return h;
}
void Place(HWND owner,int id,int x,int y,int w,int h,bool visible){
 HWND c=controls[id];const UINT dpi=GetDpiForWindow(owner);
 RECT r{};GetClientRect(owner,&r);const int inset=(std::max)(0,(int(r.right)-MulDiv(390,dpi,96))/2);
 PlaceStable(c,inset+MulDiv(x,dpi,96),MulDiv(y,dpi,96),MulDiv(w,dpi,96),MulDiv(h,dpi,96),visible);
}
void Refresh(HWND owner){
 rebuilding=true;
 for(int i=0;i<3;++i)Place(owner,Tab+i,16+i*108,14,100,32,true);
 Place(owner,ClosePanel,346,14,28,32,true);
 const bool effect=g_experienceTab==0,playlist=g_experienceTab==1,screen=g_experienceTab==2;
 const auto e=g_preferences[FocusKey(g_videoPath)].effects;
 SendMessageW(controls[Kind],CB_SETCURSEL,e.kind,0);Place(owner,Kind,16,86,340,180,effect);
 const int values[]{e.density,e.speed,e.opacity,e.brightness,e.saturation,e.warmth};
 for(int i=0;i<6;++i){
  SetWindowTextW(controls[SliderLabel+i],(std::wstring(names[i])+L"  "+std::to_wstring(values[i])).c_str());
  Place(owner,SliderLabel+i,16,132+i*49,340,19,effect);
  SendMessageW(controls[Slider+i],TBM_SETPOS,TRUE,values[i]);
  Place(owner,Slider+i,16,151+i*49,340,27,effect);
 }
 SetWindowTextW(controls[Compare],g_compareOriginal?L"正在看原画 · 返回效果":L"对比原画");
 Place(owner,Compare,16,445,214,34,effect);Place(owner,Reset,238,445,118,34,effect);
 Place(owner,Apply,16,524,340,38,!playlist);
 EnableWindow(controls[Apply],!g_videoPath.empty()&&!g_applying);
 for(int i=0;i<6;++i)EnableWindow(controls[Slider+i],!g_videoPath.empty() && (i>=3 || e.Particles()));
 Place(owner,Group,16,86,340,140,playlist);SendMessageW(controls[Group],CB_SETCURSEL,g_playlistGroup,0);
 Place(owner,List,16,126,340,230,playlist);
 if(playlist){
  ListView_DeleteAllItems(list);
  for(size_t i=0;i<g_videos.size();++i){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=g_videos[i].title.data();
   ListView_InsertItem(list,&item);ListView_SetCheckState(list,int(i),(g_preferences[FocusKey(g_videos[i].path)].playlists&(1U<<g_playlistGroup))!=0);}
 }
 const auto check=[&](int id,bool value){SendMessageW(controls[id],BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);};
 check(Enabled,g_playlistEnabled);check(Random,g_playlistRandom);check(Schedule,g_playlistSchedule);
 Place(owner,Enabled,16,368,170,26,playlist);Place(owner,Interval,202,366,154,200,playlist);
 const int minutes[]{1,5,15,30,60};for(int n=0;n<5;++n)if(g_playlistMinutes==minutes[n])SendMessageW(controls[Interval],CB_SETCURSEL,n,0);
 Place(owner,Random,16,402,340,26,playlist);Place(owner,Schedule,16,436,340,26,playlist);
 Place(owner,Next,16,488,340,34,playlist);
 Place(owner,AllScreens,16,326,340,34,screen);
 Place(owner,Accent,16,402,340,34,screen);
 Place(owner,Ball,16,446,340,34,screen);
 SetWindowTextW(controls[Accent],g_accentFromCover?L"封面强调色：开":L"封面强调色：关");
 SetWindowTextW(controls[Ball],g_ballAutoHide?L"悬浮球贴边收起：开":L"悬浮球贴边收起：关");
 rebuilding=false;InvalidateRect(owner,nullptr,FALSE);
}
void Advance(HWND owner,bool manual){
 if(g_applying||g_playlistBusy||g_sessionSuspended||(!manual&&g_wallpaperPaused))return;
 if(!manual&&g_player.channel&&g_player.channel->Data()&&InterlockedCompareExchange(&g_player.channel->Data()->energyPaused,0,0))return;
 SYSTEMTIME now{};GetLocalTime(&now);const auto groupId=CurrentPlaylist(g_playlistSchedule,g_playlistGroup,now.wHour);
 std::vector<size_t> candidates;
 for(size_t i=0;i<g_videos.size();++i)if((g_preferences[FocusKey(g_videos[i].path)].playlists&(1U<<groupId)) &&
   GetFileAttributesW(g_videos[i].path.c_str())!=INVALID_FILE_ATTRIBUTES)candidates.push_back(i);
 size_t current=SIZE_MAX;for(size_t i=0;i<g_videos.size();++i)if(IsAppliedVideo(g_videos[i].path))current=i;
 const auto next=NextPlaylistIndex(candidates,current,g_playlistRandom,GetTickCount64());
 g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;g_lastPlaylistGroup=groupId;
 if(next==SIZE_MAX){if(manual)SetStatus(owner,L"列表至少需要另一张可用壁纸，请勾选素材",kWarning);return;}
 // 自动轮换不改变用户正在浏览的素材；应用流程仍以首帧就绪为交接条件。
 const auto selected=g_selectedVideo;const auto path=g_videoPath;g_playlistBusy=true;
 g_selectedVideo=next;g_videoPath=g_videos[next].path;
 const bool applied=LaunchPlayer(owner);
 if(!manual && !path.empty()){g_selectedVideo=selected;g_videoPath=path;
  if(!g_inTray) {if(!RestoreAppliedSharedPreview(owner))ScheduleCurrentPreview(owner);}}
 else if(applied){SelectVideo(owner,next);RebuildGallery(owner,true);}
 g_playlistBusy=false;
}
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
 switch(msg){
 case WM_CREATE:{
  Control(hwnd,ClosePanel,L"BUTTON",L"×",BS_PUSHBUTTON);
  for(int i=0;i<3;++i)Control(hwnd,Tab+i,L"BUTTON",i==0?L"画面效果":i==1?L"播放列表":L"屏幕与外观",BS_PUSHBUTTON);
  auto h=Control(hwnd,Kind,L"COMBOBOX",L"",CBS_DROPDOWNLIST);
  for(const wchar_t* s:{L"无粒子",L"萤火虫",L"细雨",L"飘雪"})SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
  for(int i=0;i<6;++i){Control(hwnd,SliderLabel+i,L"STATIC",names[i],0);
   h=Control(hwnd,Slider+i,TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS);
   SendMessageW(h,TBM_SETRANGEMIN,FALSE,minimum[i]);SendMessageW(h,TBM_SETRANGEMAX,FALSE,maximum[i]);}
  Control(hwnd,Compare,L"BUTTON",L"对比原画",BS_PUSHBUTTON);Control(hwnd,Reset,L"BUTTON",L"恢复默认",BS_PUSHBUTTON);
  Control(hwnd,Apply,L"BUTTON",L"应用到桌面",BS_PUSHBUTTON);
  group=Control(hwnd,Group,L"COMBOBOX",L"",CBS_DROPDOWNLIST);
  for(const wchar_t* s:{L"常规列表",L"白天列表（07:00–19:00）",L"夜晚列表（19:00–07:00）"})SendMessageW(group,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
  list=Control(hwnd,List,WC_LISTVIEWW,L"",LVS_REPORT|LVS_NOCOLUMNHEADER|LVS_SINGLESEL);
  ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW col{};col.mask=LVCF_WIDTH;col.cx=Scale(315);ListView_InsertColumn(list,0,&col);
  ListView_SetBkColor(list,kPanel);ListView_SetTextBkColor(list,kPanel);ListView_SetTextColor(list,kText);
  Control(hwnd,Enabled,L"BUTTON",L"启用自动轮换",BS_AUTOCHECKBOX);
  h=Control(hwnd,Interval,L"COMBOBOX",L"",CBS_DROPDOWNLIST);
  for(const wchar_t* s:{L"每 1 分钟",L"每 5 分钟",L"每 15 分钟",L"每 30 分钟",L"每 60 分钟"})SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
  Control(hwnd,Random,L"BUTTON",L"随机播放（不连续重复）",BS_AUTOCHECKBOX);
  Control(hwnd,Schedule,L"BUTTON",L"按白天／夜晚自动选择列表",BS_AUTOCHECKBOX);
  Control(hwnd,Next,L"BUTTON",L"立即应用下一张",BS_PUSHBUTTON);
  Control(hwnd,AllScreens,L"BUTTON",L"应用到全部屏幕",BS_PUSHBUTTON);
  Control(hwnd,Accent,L"BUTTON",L"封面强调色",BS_PUSHBUTTON);Control(hwnd,Ball,L"BUTTON",L"悬浮球贴边收起",BS_PUSHBUTTON);
  Refresh(hwnd);return 0;
 }
 case WM_APP+1:Refresh(hwnd);return 0;
 case WM_SIZE:if(!rebuilding)Refresh(hwnd);return 0;
 case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);LayoutControls(g_mainWindow);RedrawWindow(g_mainWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;
 case WM_DESTROY:controls.clear();g_experienceWindow=nullptr;return 0;
 case WM_DPICHANGED:{const RECT* r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);Refresh(hwnd);return 0;}
 case WM_COMMAND:{
  const int id=LOWORD(wp);if(g_applying)return 0;
  if((id==Kind||id==Group||id==Interval) ? HIWORD(wp)!=CBN_SELCHANGE : HIWORD(wp)!=BN_CLICKED)return 0;
  if(id==ClosePanel){SendMessageW(hwnd,WM_CLOSE,0,0);return 0;}
  if(id>=Tab&&id<Tab+3){g_experienceTab=id-Tab;Refresh(hwnd);return 0;}
  if(id==Kind&&HIWORD(wp)==CBN_SELCHANGE){g_preferences[FocusKey(g_videoPath)].effects.kind=int(SendMessageW(controls[Kind],CB_GETCURSEL,0,0));UpdateExperience(g_mainWindow);}
  else if(id==Compare){g_compareOriginal=!g_compareOriginal;UpdateExperience(g_mainWindow);}
  else if(id==Reset){g_preferences[FocusKey(g_videoPath)].effects={};g_compareOriginal=false;UpdateExperience(g_mainWindow);}
  else if(id==Apply){g_compareOriginal=false;LaunchPlayer(g_mainWindow);}
  else if(id==Group&&HIWORD(wp)==CBN_SELCHANGE)g_playlistGroup=int(SendMessageW(group,CB_GETCURSEL,0,0));
  else if(id==Enabled)g_playlistEnabled=SendMessageW(controls[id],BM_GETCHECK,0,0)==BST_CHECKED;
  else if(id==Random)g_playlistRandom=SendMessageW(controls[id],BM_GETCHECK,0,0)==BST_CHECKED;
  else if(id==Schedule)g_playlistSchedule=SendMessageW(controls[id],BM_GETCHECK,0,0)==BST_CHECKED;
  else if(id==Interval&&HIWORD(wp)==CBN_SELCHANGE){const int options[]{1,5,15,30,60};g_playlistMinutes=options[std::clamp(int(SendMessageW(controls[id],CB_GETCURSEL,0,0)),0,4)];}
  else if(id==Next)Advance(g_mainWindow,true);
  else if(id==AllScreens){g_targetMonitor.clear();SetStatus(g_mainWindow,L"已选择全部屏幕，点击应用生效",kSuccess);}
  else if(id==Accent){g_accentFromCover=!g_accentFromCover;ApplyTheme(g_mainWindow);UpdateCoverAccent(g_mainWindow);}
  else if(id==Ball)g_ballAutoHide=!g_ballAutoHide;
  else return 0;
  if(id>=Group && id<=Schedule)g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;
  SaveConfig();Refresh(hwnd);return 0;
 }
 case WM_HSCROLL:{
  if(g_applying||g_videoPath.empty())return 0;const int index=GetDlgCtrlID(reinterpret_cast<HWND>(lp))-Slider;
  if(index<0||index>=6)return 0;
  auto& e=g_preferences[FocusKey(g_videoPath)].effects;int* fields[]{&e.density,&e.speed,&e.opacity,&e.brightness,&e.saturation,&e.warmth};
  *fields[index]=int(SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0));e.Clamp();g_compareOriginal=false;
  SetWindowTextW(controls[SliderLabel+index],(std::wstring(names[index])+L"  "+std::to_wstring(*fields[index])).c_str());
  UpdateExperience(g_mainWindow);return 0;
 }
 case WM_NOTIFY:{
  const auto* header=reinterpret_cast<NMHDR*>(lp);
  if(header->idFrom>=Slider && header->idFrom<Slider+6 && header->code==NM_CUSTOMDRAW)return DrawThemeSlider(header,kPanel);
  if(header->idFrom==List&&header->code==LVN_ITEMCHANGED&&!rebuilding&&!g_applying){
   const auto* item=reinterpret_cast<NMLISTVIEW*>(lp);
   if(item->iItem>=0&&size_t(item->iItem)<g_videos.size()&&((item->uOldState^item->uNewState)&LVIS_STATEIMAGEMASK)){
    auto& flags=g_preferences[FocusKey(g_videos[item->iItem].path)].playlists;const unsigned bit=1U<<g_playlistGroup;
    if(ListView_GetCheckState(list,item->iItem))flags|=bit;else flags&=~bit;SaveConfig();}
  }return 0;
 }
 case WM_LBUTTONUP:{if(g_experienceTab!=2||g_applying)return 0;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  for(size_t i=0;i<screenRects.size();++i)if(PtInRect(&screenRects[i],p)){
   g_targetMonitor=g_screens[i].device;g_cropMonitor=g_targetMonitor;SaveConfig();LayoutControls(g_mainWindow);
   SetStatus(g_mainWindow,L"已选择屏幕 "+std::to_wstring(i+1)+L" · 可调整取景，然后应用",kSuccess);InvalidateRect(hwnd,nullptr,TRUE);break;}return 0;}
 case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX: {
  HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,kText);SetBkColor(dc,kPanel);return reinterpret_cast<LRESULT>(g_controlBrush);
 }
 case WM_MEASUREITEM:if(auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);item->CtlType==ODT_COMBOBOX){item->itemHeight=Scale(30);return TRUE;}break;
 case WM_DRAWITEM:if(auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);item->CtlType==ODT_COMBOBOX){DrawCombo(item);return TRUE;}
  DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:{
  PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);FillRect(dc,&r,g_controlBrush);
  const auto label=[&](int y,const std::wstring& text,int h=30){RECT box{ScaleForWindow(hwnd,16),ScaleForWindow(hwnd,y),r.right-ScaleForWindow(hwnd,16),ScaleForWindow(hwnd,y+h)};
   DrawLabel(dc,text,box,g_smallFont,kMuted,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);};
  if(g_experienceTab==0){label(58,g_videoPath.empty()?L"先选择一张壁纸":std::filesystem::path(g_videoPath).stem().wstring(),24);
   label(484,g_preferences[FocusKey(g_videoPath)].effects.Particles()?
    L"只改变窗口预览；点击应用才更新桌面。\n参数按素材记忆，不改写原文件。":
    L"选择粒子后可调前三项；调色始终可用。\n这里只改预览，点击应用才更新桌面。",36);}
  else if(g_experienceTab==1){label(58,L"勾选素材加入当前列表；暂停壁纸时不轮换。");label(536,L"空列表不打断当前壁纸。锁屏／睡眠停止轮换，恢复后不追赶漏过的切换。",48);}
  else {
   label(60,L"点击屏幕：选择应用目标及取景屏幕。",40);
   RECT bounds{};for(const auto& s:g_screens)UnionRect(&bounds,&bounds,&s.rect);
   const double scale=(std::min)(double(ScaleForWindow(hwnd,328))/(std::max)(1L,bounds.right-bounds.left),double(ScaleForWindow(hwnd,190))/(std::max)(1L,bounds.bottom-bounds.top));
   screenRects.clear();for(size_t i=0;i<g_screens.size();++i){const auto& s=g_screens[i];
    RECT box{ScaleForWindow(hwnd,22)+LONG((s.rect.left-bounds.left)*scale),ScaleForWindow(hwnd,116)+LONG((s.rect.top-bounds.top)*scale),
     ScaleForWindow(hwnd,22)+LONG((s.rect.right-bounds.left)*scale),ScaleForWindow(hwnd,116)+LONG((s.rect.bottom-bounds.top)*scale)};
    InflateRect(&box,-3,-3);screenRects.push_back(box);const bool selected=g_targetMonitor.empty()||g_targetMonitor==s.device;
    FillRounded(dc,box,12,selected?kAccent:kControlSurface);DrawLabel(dc,std::to_wstring(i+1)+(s.primary?L" 主屏":L""),box,g_bodyFont,selected?RGB(255,255,255):kText,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
   label(368,ScreenLabel(g_targetMonitor,true));label(490,L"同一素材共享一次解码，每块屏幕独立取景。",30);
  }
  EndPaint(hwnd,&ps);return 0;
 }
 }return DefWindowProcW(hwnd,msg,wp,lp);
}
}

void RefreshExperienceTheme(){
 if(Experience::list){ListView_SetBkColor(Experience::list,kPanel);ListView_SetTextBkColor(Experience::list,kPanel);ListView_SetTextColor(Experience::list,kText);}
 if(g_experienceWindow)RedrawWindow(g_experienceWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}

std::wstring SaveExperience(){
 std::wstring text=L"\r\n[Experience]\r\nFilter="+std::to_wstring(g_libraryFilter)+L"\r\nCoverAccent="+(g_accentFromCover?L"1":L"0")+
  L"\r\nSplit="+std::to_wstring(int(g_previewRatio*100))+L"\r\nBallSize="+std::to_wstring(g_ballSize)+
  L"\r\nBallDock="+(g_ballAutoHide?L"1":L"0")+L"\r\nPlaylist="+(g_playlistEnabled?L"1":L"0")+
  L"\r\nGroup="+std::to_wstring(g_playlistGroup)+L"\r\nMinutes="+std::to_wstring(g_playlistMinutes)+
  L"\r\nRandom="+(g_playlistRandom?L"1":L"0")+L"\r\nSchedule="+(g_playlistSchedule?L"1":L"0")+L"\r\n\r\n[Materials]\r\n";
 for(const auto& [key,p]:g_preferences)if(p.favorite||p.lastUsed||p.playlists||p.effects.Enabled())text+=key+L"="+p.Encode()+L"\r\n";
 text+=L"\r\n[External]\r\n";for(const auto& path:g_externalFiles)text+=FocusKey(path)+L"="+path+L"\r\n";
 return text;
}
void LoadExperience(){
 const auto number=[](const wchar_t* key,int fallback,int low,int high){const auto v=LoadConfigValue(L"Experience",key);return v.empty()?fallback:std::clamp(_wtoi(v.c_str()),low,high);};
 g_libraryFilter=number(L"Filter",0,0,2);g_accentFromCover=number(L"CoverAccent",0,0,1)!=0;
 g_previewRatio=number(L"Split",60,45,75)/100.0;g_ballSize=number(L"BallSize",116,92,148);g_ballAutoHide=number(L"BallDock",0,0,1)!=0;
 g_playlistEnabled=number(L"Playlist",0,0,1)!=0;g_playlistGroup=number(L"Group",0,0,2);g_playlistMinutes=number(L"Minutes",15,1,60);
 g_playlistRandom=number(L"Random",0,0,1)!=0;g_playlistSchedule=number(L"Schedule",0,0,1)!=0;
 std::vector<wchar_t> section(1024*1024);
 for(const auto* name:{L"Materials",L"External"}){
  GetPrivateProfileSectionW(name,section.data(),DWORD(section.size()),ConfigPath().c_str());
  for(const wchar_t* line=section.data();*line;line+=wcslen(line)+1){std::wstring field=line;const auto eq=field.find(L'=');
   if(eq!=16)continue;if(wcscmp(name,L"Materials")==0)g_preferences[field.substr(0,eq)]=LibraryPreference::Decode(field.substr(eq+1));
   else if(g_externalFiles.size()<1000)g_externalFiles.push_back(field.substr(eq+1));}
 }
 g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;
}
void StopEffectPreview(){
 if(g_effectPreview.process){g_previewActive=false;g_previewLoading=false;}
 if(g_effectPreview.process)StopPlayer(g_effectPreview,false);
 if(g_effectPreviewWindow){ShowWindow(g_effectPreviewWindow,SW_HIDE);DestroyWindow(g_effectPreviewWindow);g_effectPreviewWindow=nullptr;}
 if(g_mainWindow)KillTimer(g_mainWindow,kEffectPreviewTimer);
}
bool StartEffectPreview(HWND owner){
 StopEffectPreview();StopSharedPreview(owner);if(g_previewPlayer)g_previewPlayer->Stop();g_previewRequest=0;
 g_previewActive=false;g_previewLoading=true;
 g_effectPreviewWindow=CreateWindowW(L"DWDSharedPreviewSurface",nullptr,WS_CHILD,0,0,1,1,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
 LayoutControls(owner);
 PlayerInstance next;const auto name=L"Local\\DWD_Effects_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(++g_generation);
 const auto stop=name+L"_Stop",ready=name+L"_Ready",pause=name+L"_Pause";
 next.stopEvent=CreateEventW(nullptr,TRUE,FALSE,stop.c_str());next.readyEvent=CreateEventW(nullptr,TRUE,FALSE,ready.c_str());next.pauseEvent=CreateEventW(nullptr,TRUE,FALSE,pause.c_str());
 next.channel=std::make_shared<EffectChannel>();next.videoPath=g_videoPath;
 if(!next.stopEvent||!next.readyEvent||!next.pauseEvent||!next.channel->Open(name,true)){CloseHandles(next);return false;}
 next.channel->Write(CurrentEffects());
 std::wstring path=g_videoPath;
 if(!IsWallpaperImage(path)&&g_selectedVideo<g_videos.size()){const auto& v=g_videos[g_selectedVideo];const auto proxy=BuildProxySpec(path,v.width,v.height);if(ProxyFileReady(proxy))path=proxy.finalPath;}
 const auto exe=ExeDirectory()+L"\\DWDNative.exe";
 std::wstring command=Quote(exe)+L" --player --video "+Quote(path)+L" --stop-event "+Quote(stop)+L" --ready-event "+Quote(ready)+
  L" --pause-event "+Quote(pause)+L" --parent-pid "+std::to_wstring(GetCurrentProcessId())+L" --effects-channel "+Quote(name)+
  L" --preview-window "+std::to_wstring(reinterpret_cast<UINT_PTR>(g_effectPreviewWindow));
 STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
 if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,ExeDirectory().c_str(),&startup,&process)){
  CloseHandles(next);return false;}
 CloseHandle(process.hThread);next.process=process.hProcess;g_effectPreview=std::move(next);
 g_effectPreviewDeadline=GetTickCount64()+12000;SetTimer(owner,kEffectPreviewTimer,50,nullptr);return true;
}
void UpdateExperience(HWND owner){
 SaveConfig();const auto effects=CurrentEffects();
 if(g_sharedPreviewActive&&g_sharedPreview){g_sharedPreview->SetEffects(effects,CurrentVideoSize());g_sharedPreview->Render();}
 else if(g_effectPreview.process&&g_effectPreview.channel)g_effectPreview.channel->Write(effects);
 else if(!g_inTray&&!g_videoPath.empty()&&effects.Enabled()&&!g_previewLoading)ScheduleCurrentPreview(owner,true);
 SetStatus(owner,g_compareOriginal?L"正在对比原画 · 桌面未改变":L"预览效果已保存 · 点击应用后更新桌面",kMuted);
}
void ShowExperience(HWND owner,int tab){
 if(g_settingsOpen)SetSettingsOpen(owner,false);g_experienceTab=tab;
 if(!g_experienceWindow){WNDCLASSW cls{};cls.lpfnWndProc=Experience::Proc;cls.hInstance=GetModuleHandleW(nullptr);
  cls.lpszClassName=L"DWDEffectsPanel";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
  RECT r{};GetClientRect(owner,&r);const int width=Scale(390);
  // 停靠在图库一侧，保留左侧实时预览；不新开任务栏窗口，也不会跑到另一块屏幕。
  g_experienceWindow=CreateWindowExW(0,cls.lpszClassName,L"DWD · 画面与播放",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
   r.right-width,0,width,r.bottom,owner,nullptr,cls.hInstance,nullptr);
 }
 Experience::Refresh(g_experienceWindow);ShowWindow(g_experienceWindow,SW_SHOWNORMAL);LayoutControls(owner);
}
void UpdateCoverAccent(HWND owner){
 if(!g_accentFromCover || g_selectedVideo>=g_videos.size() || !g_videos[g_selectedVideo].thumbnail)return;
 // 只在选择/封面到达时采一次缩小后的封面，不每帧分析视频。
 HBITMAP bitmap=g_videos[g_selectedVideo].thumbnail;BITMAP info{};GetObjectW(bitmap,sizeof(info),&info);
 if(info.bmWidth<=0||info.bmHeight<=0||info.bmWidth>512||info.bmHeight>512)return;
 std::vector<BYTE> pixels(size_t(info.bmWidth)*info.bmHeight*4);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
 bi.bmiHeader.biWidth=info.bmWidth;bi.bmiHeader.biHeight=-info.bmHeight;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 HDC dc=GetDC(owner);const bool ok=GetDIBits(dc,bitmap,0,info.bmHeight,pixels.data(),&bi,DIB_RGB_COLORS)!=0;ReleaseDC(owner,dc);if(!ok)return;
 uint64_t r{},g{},b{},count{};for(size_t n=0;n+3<pixels.size();n+=4*17){b+=pixels[n];g+=pixels[n+1];r+=pixels[n+2];++count;}
 if(!count)return;const int maximum=BackgroundIsLight()?145:205,minimum=BackgroundIsLight()?40:90;
 kAccent=RGB(std::clamp(int(r/count),minimum,maximum),std::clamp(int(g/count),minimum,maximum),std::clamp(int(b/count),minimum,maximum));
 kAccentHover=RGB((GetRValue(kAccent)+80)/2,(GetGValue(kAccent)+100)/2,(GetBValue(kAccent)+210)/2);
 RedrawWindow(owner,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
void AcceptWallpaperFiles(HWND owner,const std::vector<std::wstring>& paths){
 if(g_applying)return;size_t first=SIZE_MAX;
 for(const auto& path:paths){
  const DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES)continue;
  if(attr&FILE_ATTRIBUTE_DIRECTORY){ScanWallpaperFolder(owner,path,true);return;}
  if(!LibraryScanner::IsVideo(path))continue;
  auto found=std::find_if(g_videos.begin(),g_videos.end(),[&](const auto& v){return _wcsicmp(v.path.c_str(),path.c_str())==0;});
  if(found==g_videos.end()){VideoItem item;item.path=path;item.title=std::filesystem::path(path).stem().wstring();item.searchTitle=item.title;
   std::transform(item.searchTitle.begin(),item.searchTitle.end(),item.searchTitle.begin(),towlower);g_videos.push_back(std::move(item));
   if(g_externalFiles.size()<1000)g_externalFiles.push_back(path);if(first==SIZE_MAX)first=g_videos.size()-1;
  }else if(first==SIZE_MAX)first=size_t(found-g_videos.begin());
 }
 if(first==SIZE_MAX){SetStatus(owner,L"支持常见视频，以及 JPG、PNG、BMP 图片；也可拖入文件夹",kWarning);return;}
 g_libraryFilter=0;SetWindowTextW(g_searchEdit,L"");g_searchText.clear();SaveConfig();RebuildGallery(owner);SelectVideo(owner,first);
}
bool ExperienceCommand(HWND owner,int id){
 using namespace Experience;
 if(id==kFavorite){if(!g_videoPath.empty()){auto& p=g_preferences[FocusKey(g_videoPath)];p.favorite=!p.favorite;SaveConfig();RebuildGallery(owner,true);}return true;}
 if(id==kLibraryFilter){g_libraryFilter=(g_libraryFilter+1)%3;RebuildGallery(owner);SaveConfig();return true;}
 if(id==kEffectsButton){ShowExperience(owner,0);return true;}
 if(id==kMoreButton){HMENU menu=CreatePopupMenu();
  AppendMenuW(menu,MF_STRING,Open,L"打开视频或图片…");AppendMenuW(menu,MF_STRING,Locate,L"重新定位当前素材…");
  AppendMenuW(menu,MF_STRING,PreviewPause,g_previewOnlyPaused?L"继续窗口预览":L"暂停窗口预览");
  AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,Tab+1,L"播放列表…");AppendMenuW(menu,MF_STRING,Tab+2,L"屏幕与外观…");
  AppendMenuW(menu,MF_STRING,kStopButton,L"恢复静态壁纸");
  POINT p{};GetCursorPos(&p);const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,owner,nullptr);DestroyMenu(menu);
  if(command)PostMessageW(owner,WM_COMMAND,command,0);return true;}
 if(id==Tab+1||id==Tab+2){ShowExperience(owner,id-Tab);return true;}
 if(id==Open||id==Locate){
  wchar_t file[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=owner;dialog.lpstrFile=file;dialog.nMaxFile=32768;
  dialog.lpstrFilter=L"视频与图片\0*.mp4;*.m4v;*.mov;*.wmv;*.mkv;*.avi;*.jpg;*.jpeg;*.png;*.bmp\0所有文件\0*.*\0";
  dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;dialog.lpstrTitle=id==Locate?L"重新定位（原文件不会被修改）":L"选择视频或图片";
  if(GetOpenFileNameW(&dialog)){
   const auto oldPath=g_videoPath;const auto pref=g_preferences[FocusKey(oldPath)];
   if(id==Locate&&!oldPath.empty()){g_preferences[FocusKey(file)]=pref;
    const auto focus=g_savedFocus.find(FocusKey(oldPath));if(focus!=g_savedFocus.end())g_savedFocus[FocusKey(file)]=focus->second;
    for(const auto& s:g_screens){const auto f=g_savedFocus.find(FocusKey(oldPath)+L"_"+FocusKey(s.device));
     if(f!=g_savedFocus.end())g_savedFocus[FocusKey(file)+L"_"+FocusKey(s.device)]=f->second;}
    if(g_lastWallpaper==oldPath)g_lastWallpaper=file;
    std::erase(g_externalFiles,oldPath);
   }
   AcceptWallpaperFiles(owner,{file});
  }return true;
 }
 if(id==PreviewPause){
  if(g_sharedPreviewActive){SetStatus(owner,L"此预览与桌面共享播放；使用“暂停壁纸”可同时暂停",kMuted);return true;}
  g_previewOnlyPaused=!g_previewOnlyPaused;
  if(g_effectPreview.pauseEvent){if(g_previewOnlyPaused)SetEvent(g_effectPreview.pauseEvent);else ResetEvent(g_effectPreview.pauseEvent);}
  else if(g_previewPlayer){if(g_previewOnlyPaused)g_previewPlayer->Pause();else g_previewPlayer->Play();}return true;
 }
 return false;
}
RECT SplitterRect(HWND owner){const auto preview=PreviewRect(owner);return RECT{preview.right+Scale(3),preview.top,preview.right+Scale(15),preview.bottom};}
void TickExperience(HWND owner){
 if(g_effectPreview.process&&!g_previewLoading&&WaitForSingleObject(g_effectPreview.process,0)==WAIT_OBJECT_0){
  StopEffectPreview();g_previewActive=false;SetStatus(owner,L"效果预览已停止 · 可重新选择素材或关闭效果重试",kError);LayoutControls(owner);
 }
 if(g_playlistEnabled&&!g_applying&&!g_playlistBusy&&!g_sessionSuspended){SYSTEMTIME time{};GetLocalTime(&time);
  const auto group=CurrentPlaylist(g_playlistSchedule,g_playlistGroup,time.wHour);
  if(!g_nextPlaylist)g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;
  if(GetTickCount64()>=g_nextPlaylist || (g_playlistSchedule&&group!=g_lastPlaylistGroup))Experience::Advance(owner,false);
 }
 if(g_inTray||g_settingsOpen||IsWallpaperImage(g_videoPath))return;
 int position{},duration{};
 if(g_effectPreview.channel&&g_effectPreview.channel->Data()){auto* data=g_effectPreview.channel->Data();
  position=InterlockedCompareExchange(&data->positionMs,0,0);duration=InterlockedCompareExchange(&data->durationMs,0,0);
 }else if(g_sharedPreviewActive && g_player.channel && g_player.channel->Data()){
  auto* data=g_player.channel->Data();position=InterlockedCompareExchange(&data->positionMs,0,0);duration=InterlockedCompareExchange(&data->durationMs,0,0);
 }else if(g_previewPlayer){g_previewPlayer->PollPosition();position=g_previewPlayer->PositionMs();duration=g_previewPlayer->DurationMs();}
 EnableWindow(g_seekBar,duration>0&&!g_applying);
 if(!g_seekDragging&&duration>0)SendMessageW(g_seekBar,TBM_SETPOS,TRUE,(std::min)(1000,int(int64_t(position)*1000/duration)));
 if(g_pendingSeek>=0&&duration>0){const int target=int(int64_t(duration)*g_pendingSeek/1000);
  if(g_effectPreview.channel&&g_effectPreview.channel->Data())InterlockedExchange(&g_effectPreview.channel->Data()->seekMs,target);
  else if(g_previewPlayer&&!g_sharedPreviewActive)g_previewPlayer->Seek(target);
  g_pendingSeek=-1;
 }
}
