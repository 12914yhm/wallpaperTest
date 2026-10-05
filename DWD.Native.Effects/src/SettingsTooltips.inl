// 设置提示统一使用已有的 Win32 tooltip；文字按当前选项生成，配置变化后无需重建控件。
namespace SettingTooltips {
std::map<HWND,int> popupCombos;
std::wstring activeText;

std::wstring Option(int id,int index) {
    if(id==kQuality){
        switch(index){
        case 0:return L"均衡：只在源视频明显大于屏幕时生成较小的优化视频；其他视频直接播放原文件。";
        case 1:return L"原视频：直接播放源文件，不生成优化视频；高分辨率视频可能需要更多解码资源。";
        case 2:return L"节能：按需生成最高 1080p、30 帧的优化视频；画面细节和帧率可能降低。";}
    }
    if(id==kFullscreenRule){
        switch(index){
        case 0:return L"任一屏全屏就暂停：有应用在任意目标屏幕全屏时，暂停全部动态壁纸。";
        case 1:return L"全部目标屏幕被全屏覆盖才暂停：还能看见桌面的屏幕会继续播放。";
        case 2:return L"关闭全屏暂停：全屏应用不会让壁纸暂停；锁屏和睡眠时仍暂停。";}
    }
    if(id==kScreenTarget)return index==0?L"全部屏幕：把同一张壁纸应用到所有屏幕，每块屏幕可分别取景。":
        L"只应用到屏幕 "+std::to_wstring(index)+L"；其他屏幕不播放这张动态壁纸。";
    if(id==kCropMonitor)return L"选择屏幕 "+std::to_wstring(index+1)+L" 的取景区域；调整主体后点击“应用到桌面”生效。";
    if(id==kThemeFirst){
        switch(index){
        case 0:return L"深色：使用深色应用界面，不改变桌面壁纸。";
        case 1:return L"浅色：使用明亮的应用界面，不改变桌面壁纸。";
        case 2:return L"跟随系统：读取 Windows 的深浅色偏好，不修改系统设置。";
        case 3:return L"自定义背景颜色：打开调色器设置应用界面的纯色背景，不是选择背景图片。";}
    }
    if(id==kCacheLimit && index>=0 && index<4)return
        L"优化视频缓存最多约 "+std::to_wstring(512<<index)+L" MB；超出时按最近使用情况清理，正在播放的文件会保留。";
    if(id==kSortCombo){
        switch(index){case 0:return L"按素材名称从 A 到 Z 排列。";
        case 1:return L"按素材名称从 Z 到 A 排列。";
        case 2:return L"最近修改的素材排在前面。";}
    }
    return {};
}

std::wstring Hint(int id,HWND control) {
    if(id==kQuality||id==kFullscreenRule||id==kScreenTarget||id==kCropMonitor||id==kThemeFirst||id==kCacheLimit||id==kSortCombo)
        return Option(id,int(SendMessageW(control,CB_GETCURSEL,0,0)));
    switch(id){
    case kSettingsButton:return L"展开或收起设置。这里可以更改壁纸来源、播放、外观、缓存和使用时长统计。";
    case kTabFirst:return L"壁纸：选择素材文件夹，设置启动恢复与每块屏幕的取景位置。";
    case kTabFirst+1:return L"播放：选择画质、全屏暂停方式和壁纸应用到哪些屏幕。";
    case kTabFirst+2:return L"外观：选择界面主题、背景颜色和内存悬浮球的显示方式。";
    case kTabFirst+3:return L"缓存：选择缓存文件夹、空间上限，并管理 DWD 生成的缓存文件。";
    case kTabFirst+4:return L"统计：选择使用时长的统计口径，控制是否记录。";
    case kChooseFolderButton:return L"选择存放视频或图片的文件夹。DWD 会读取其中素材，不移动原文件。";
    case kAutoRestore:return L"开启后，下次启动 DWD 会恢复上次应用的壁纸；这不是 Windows 开机自启动。";
    case kResetCropButton:return L"把当前素材在所选屏幕上的取景位置恢复到画面中央；重新应用壁纸后生效。";
    case kChooseBackgroundButton:return L"打开 Windows 调色器，选择应用界面的背景颜色；不改变桌面壁纸。";
    case kBallToggle:return L"显示或隐藏内存悬浮球。隐藏后会停止悬浮球的内存采样。";
    case kChooseCacheButton:return L"选择 DWD 封面和优化视频的保存位置；原素材仍留在原文件夹。";
    case kClearThumbnails:return L"清理 DWD 生成的封面缓存；下次浏览时会重新生成。";
    case kClearProxies:return L"清理不在使用中的优化视频缓存；不会删除原始视频。";
    case kUsageToggle:return L"开启或关闭 DWD 运行期间的使用时长统计。";
    case kActiveModeButton:return L"活跃时长：前台应用超过 60 秒没有键鼠输入后，暂停计时。";
    case kViewingModeButton:return L"观看时长：应用保持前台就累计；锁屏和睡眠时暂停。";
    default:return {};
    }
}

void Add(HWND control) {
    if(!g_tooltip||!control)return;
    TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
    tool.hwnd=g_mainWindow;tool.uId=reinterpret_cast<UINT_PTR>(control);
    tool.lpszText=LPSTR_TEXTCALLBACKW;
    SendMessageW(g_tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
}

void Register() {
    if(!g_tooltip)return;
    Add(g_settingsButton);
    for(int id:{kChooseFolderButton,kChooseCacheButton,kChooseBackgroundButton,kActiveModeButton,kViewingModeButton,kSortCombo}) {
        HWND control=id==kChooseFolderButton?g_chooseFolderButton:id==kChooseCacheButton?g_chooseCacheButton:
            id==kChooseBackgroundButton?g_chooseBackgroundButton:id==kActiveModeButton?g_activeModeButton:
            id==kViewingModeButton?g_viewingModeButton:g_sortCombo;
        Add(control);
    }
    for(const auto& [id,control]:g_extraButtons) {
        // 下拉框此时可能尚未设置当前项；仍要先注册，提示文字在悬停时实时计算。
        if(Hint(id,control).empty() && !IsSettingCombo(id))continue;
        Add(control);
        if(!IsCombo(control))continue;
        COMBOBOXINFO info{sizeof(info)};
        if(GetComboBoxInfo(control,&info) && info.hwndList){popupCombos[info.hwndList]=id;Add(info.hwndList);}
    }
    if(g_sortCombo){COMBOBOXINFO info{sizeof(info)};
        if(GetComboBoxInfo(g_sortCombo,&info)&&info.hwndList){popupCombos[info.hwndList]=kSortCombo;Add(info.hwndList);}}
}

bool Notify(const NMHDR* header) {
    if(!g_tooltip||header->hwndFrom!=g_tooltip||header->code!=TTN_GETDISPINFOW)return false;
    auto* info=reinterpret_cast<NMTTDISPINFOW*>(const_cast<NMHDR*>(header));
    HWND control=reinterpret_cast<HWND>(header->idFrom);
    const auto found=popupCombos.find(control);
    if(found!=popupCombos.end()) {
        POINT point{};GetCursorPos(&point);ScreenToClient(control,&point);
        const LRESULT item=SendMessageW(control,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
        activeText=HIWORD(item)?L"":Option(found->second,LOWORD(item));
    } else activeText=Hint(GetDlgCtrlID(control),control);
    info->lpszText=activeText.empty()?const_cast<LPWSTR>(L""):activeText.data();
    return true;
}
}
