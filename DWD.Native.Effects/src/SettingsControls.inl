// 原生控件保留键盘、鼠标和辅助功能行为，只统一绘制，不引入 UI 框架。
bool IsCombo(HWND control) {
    wchar_t name[32]{}; GetClassNameW(control,name,32);
    return _wcsicmp(name,L"ComboBox")==0;
}

// 不变的位置不重复 MoveWindow；否则每次按钮通知都会让视频和侧栏重绘。
void PlaceStable(HWND control,int x,int y,int width,int height,bool show) {
    if(!control)return;
    RECT old{};GetWindowRect(control,&old);MapWindowPoints(nullptr,GetParent(control),reinterpret_cast<POINT*>(&old),2);
    if(old.left!=x || old.top!=y || old.right-old.left!=width || (!IsCombo(control)&&old.bottom-old.top!=height))
        SetWindowPos(control,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    const bool visible=(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)!=0;
    if(visible!=show)ShowWindow(control,show?SW_SHOWNA:SW_HIDE);
}

void DrawCombo(const DRAWITEMSTRUCT* item) {
    if(!item || !item->hwndItem)return;
    const bool highlight=(item->itemState&ODS_SELECTED) && !(item->itemState&ODS_COMBOBOXEDIT);
    HBRUSH brush=CreateSolidBrush(highlight?kAccent:kPanel);FillRect(item->hDC,&item->rcItem,brush);DeleteObject(brush);
    if(item->itemID==UINT(-1))return;
    const auto length=SendMessageW(item->hwndItem,CB_GETLBTEXTLEN,item->itemID,0);
    if(length<0 || length>4096)return;
    std::wstring text(size_t(length)+1,L'\0');SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,reinterpret_cast<LPARAM>(text.data()));
    RECT r=item->rcItem;InflateRect(&r,-Scale(10),0);
    DrawLabel(item->hDC,text.c_str(),r,g_bodyFont,(item->itemState&ODS_DISABLED)?kMuted:highlight?RGB(255,255,255):kText,
              DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
}

LRESULT CALLBACK ComboPaintProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);
        FillRect(dc,&r,g_controlBrush);HBRUSH border=CreateSolidBrush(GetFocus()==hwnd?kAccent:kBorder);FrameRect(dc,&r,border);DeleteObject(border);
        wchar_t label[512]{};GetWindowTextW(hwnd,label,512);RECT text=r;text.left+=Scale(12);text.right-=Scale(30);
        DrawLabel(dc,label,text,g_bodyFont,IsWindowEnabled(hwnd)?kText:kMuted,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        const int x=r.right-Scale(16),y=(r.bottom+r.top)/2;
        HPEN pen=CreatePen(PS_SOLID,Scale(2),kMuted);auto old=SelectObject(dc,pen);
        MoveToEx(dc,x-Scale(4),y-Scale(2),nullptr);LineTo(dc,x,y+Scale(2));LineTo(dc,x+Scale(4),y-Scale(2));
        SelectObject(dc,old);DeleteObject(pen);EndPaint(hwnd,&ps);return 0;
    }
    const auto result=DefSubclassProc(hwnd,message,wp,lp);
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==CB_SETCURSEL||message==WM_ENABLE)InvalidateRect(hwnd,nullptr,FALSE);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,ComboPaintProc,1);
    return result;
}

HWND MakeThemeCombo(HWND owner,int id,HFONT font) {
    HWND h=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_TABSTOP|WS_VSCROLL|WS_CLIPSIBLINGS|
        CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,0,0,0,0,owner,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    SendMessageW(h,CB_SETITEMHEIGHT,WPARAM(-1),Scale(30));SendMessageW(h,CB_SETITEMHEIGHT,0,Scale(30));
    SetWindowSubclass(h,ComboPaintProc,1,0);return h;
}

// 一次画完整个滑杆，避开系统主题白底、三角滑块和焦点白框；输入仍由 TRACKBAR 处理。
LRESULT DrawThemeSlider(const NMHDR* header,COLORREF background) {
    if(header->code!=NM_CUSTOMDRAW)return CDRF_DODEFAULT;
    const auto* paint=reinterpret_cast<const NMCUSTOMDRAW*>(header);
    if(paint->dwDrawStage!=CDDS_PREPAINT)return CDRF_DODEFAULT;
    RECT r{};GetClientRect(header->hwndFrom,&r);HBRUSH brush=CreateSolidBrush(background);FillRect(paint->hdc,&r,brush);DeleteObject(brush);
    RECT channel{},thumb{};SendMessageW(header->hwndFrom,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
    SendMessageW(header->hwndFrom,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
    const int cy=(r.top+r.bottom)/2,cx=(thumb.left+thumb.right)/2;
    channel.top=cy-Scale(2);channel.bottom=cy+Scale(2);FillRounded(paint->hdc,channel,Scale(4),kBorder);
    const bool enabled=IsWindowEnabled(header->hwndFrom)!=FALSE;
    RECT progress=channel;progress.right=cx;FillRounded(paint->hdc,progress,Scale(4),enabled?kAccent:kMuted);
    const int radius=Scale(GetFocus()==header->hwndFrom?7:6);
    Gdiplus::Graphics graphics(paint->hdc);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const auto color=enabled?kAccent:kMuted;
    Gdiplus::SolidBrush knob(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
    graphics.FillEllipse(&knob,cx-radius,cy-radius,radius*2,radius*2);return CDRF_SKIPDEFAULT;
}

LRESULT CALLBACK SliderPaintProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    const auto result=DefSubclassProc(hwnd,message,wp,lp);
    // 默认控件只使滑块的新旧小区域失效；自绘抗锯齿圆点需要整条重绘，避免留下断续轨迹。
    if(message==TBM_SETPOS || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_KEYDOWN ||
        message==WM_LBUTTONDOWN || (message==WM_MOUSEMOVE && GetCapture()==hwnd))InvalidateRect(hwnd,nullptr,FALSE);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,SliderPaintProc,1);
    return result;
}

bool IsSettingCombo(int id) {
    return id==kQuality||id==kFullscreenRule||id==kScreenTarget||id==kCropMonitor||id==kCacheLimit||id==kThemeFirst;
}
void SyncSettingCombos() {
    const auto select=[](int id,int value){const auto found=g_extraButtons.find(id);if(found!=g_extraButtons.end() &&
        SendMessageW(found->second,CB_GETCURSEL,0,0)!=value)SendMessageW(found->second,CB_SETCURSEL,value,0);};
    select(kQuality,g_quality);select(kFullscreenRule,g_fullscreenRule);select(kThemeFirst,g_theme);
    const uint64_t mb=g_cacheLimitBytes/(1024*1024);select(kCacheLimit,mb==512?0:mb==1024?1:mb==4096?3:2);
    for(int id:{kScreenTarget,kCropMonitor}){
        const auto found=g_extraButtons.find(id);if(found==g_extraButtons.end())continue;
        const bool all=id==kScreenTarget;std::vector<std::wstring> labels;
        if(all)labels.push_back(L"全部屏幕");
        const auto& device=all?g_targetMonitor:g_cropMonitor;int selected=0;
        for(size_t n=0;n<g_screens.size();++n){labels.push_back(ScreenLabel(g_screens[n].device,false));if(g_screens[n].device==device)selected=int(n)+(all?1:0);}
        HWND combo=found->second;bool changed=SendMessageW(combo,CB_GETCOUNT,0,0)!=LRESULT(labels.size());
        for(size_t n=0;!changed&&n<labels.size();++n){wchar_t text[1024]{};
            if(SendMessageW(combo,CB_GETLBTEXTLEN,n,0)>=1024){changed=true;break;}
            SendMessageW(combo,CB_GETLBTEXT,n,reinterpret_cast<LPARAM>(text));changed=labels[n]!=text;}
        if(changed){SendMessageW(combo,CB_RESETCONTENT,0,0);for(const auto& label:labels)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
        select(id,selected);
    }
}

std::wstring QualityDescription() {
    if(g_quality==1)return L"直接播放原文件，保留原始画质，不生成优化视频。高分辨率素材可能占用更多解码内存。";
    if(g_quality==2)return L"按需生成最高 1080p、30 帧的优化视频，优先降低播放负担；细节和流畅度可能降低。";
    return L"仅优化明显高于目标屏幕分辨率的视频，兼顾细节与播放负担；其他素材直接使用原文件。";
}
std::wstring FullscreenDescription() {
    if(g_fullscreenRule==1)return L"只有全部目标屏幕都被全屏应用覆盖才暂停；仍露出的桌面继续播放。";
    if(g_fullscreenRule==2)return L"不因全屏应用暂停壁纸。锁屏、睡眠时仍暂停。";
    return L"任意屏幕出现全屏应用时暂停全部动态壁纸，减少后台资源占用。";
}
std::wstring ThemeDescription() {
    if(g_theme==3){wchar_t color[16]{};swprintf_s(color,L"#%02X%02X%02X",GetRValue(g_customBackground),GetGValue(g_customBackground),GetBValue(g_customBackground));
        return std::wstring(L"自定义应用背景色：")+color+L"。点击下方按钮修改，文字和控件会自动调整对比度。这不是壁纸或背景图片。";}
    if(g_theme==2)return L"跟随 Windows 的浅色／深色主题；只读取系统偏好，不修改系统设置。";
    return g_theme==1?L"使用明亮的应用界面，不改变桌面壁纸。":L"使用深色的应用界面，不改变桌面壁纸。";
}

void RepaintSettings(HWND owner) {
    RECT drawer{0,0,Scale(430),0};RECT client{};GetClientRect(owner,&client);drawer.bottom=client.bottom;
    RedrawWindow(owner,&drawer,nullptr,RDW_INVALIDATE|RDW_NOCHILDREN);
}
