/*
 AmigaCloudConfig - OS3.1 + MUI 3.8 - GCC 2.95 (no ixemul)

 Features:
  - Tabs grayed when handlers missing (Devs:Cloud/#?.68k and #?_102e.68k).
  - Apply under Binary cycle.
  - Keyfile presence indicator.
  - No probing of GOOGLE: / DBOX: at startup (no requesters).
  - Safe in-place mountlist update (tmp + .bak + rollback).
  - Wider token fields (~45 chars) via MUIA_FixWidthTxt.
  - Variant pre-selection from mountlist (68k / 102e).
  - Save / Mount disabled when token is empty.
  - Logs as list with autoscroll.
  - ASL Load... to populate token field from a text file.
  - Fix warnings by using APTR for pr_WindowPtr saves/restores.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/dos.h>

#include <dos/dosextens.h>  /* struct Process, pr_WindowPtr (APTR) */

#include <libraries/mui.h>
#include <proto/muimaster.h>

#include <libraries/asl.h>
#include <proto/asl.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>   /* sprintf for IoErr logging */

struct Library *MUIMasterBase = NULL;
struct Library *AslBase       = NULL;

/* ASCII helpers */
#define CH_COLON 58   /* : */
#define CH_SLASH 47   /* / */
static int is_ws_or_nl(unsigned char c) {
    return (c==9 || c==10 || c==13 || c==32);
}

/* IDs */
#define ID_QUIT        1000
#define ID_UPDATE_BTNS 1001

#define ID_GD_SAVE     1101
#define ID_GD_PURGE    1102
#define ID_GD_MNT      1103
#define ID_GD_UMNT     1104
#define ID_GD_LOAD     1105

#define ID_DB_SAVE     1201
#define ID_DB_PURGE    1202
#define ID_DB_MNT      1203
#define ID_DB_UMNT     1204
#define ID_DB_LOAD     1205

#define ID_APPLY_BIN   1301

/* Paths */
#define PATH_CLOUD_MOUNTLIST   "Devs:Cloud/cloud.mountlist"
#define PATH_CLOUD_TMP         "Devs:Cloud/.cloud.mountlist.tmp"
#define PATH_CLOUD_BAK         "Devs:Cloud/cloud.mountlist.bak"

#define PATH_GD_CLIENT_CODE    "Devs:Cloud/google_drive_client_code"
#define PATH_GD_ACCESS_TOKEN   "Devs:Cloud/google_drive_access_token"
#define PATH_GD_REFRESH_TOKEN  "Devs:Cloud/google_drive_refresh_token"
#define PATH_DB_CLIENT_CODE    "Devs:Cloud/dropbox_client_code"
#define PATH_DB_ACCESS_TOKEN   "Devs:Cloud/dropbox_access_token"

/* Handler filenames */
#define FN_DB_68K   "dropbox-handler.68k"
#define FN_DB_102E  "dropbox-handler_102e.68k"
#define FN_GD_68K   "google-drive-handler.68k"
#define FN_GD_102E  "google-drive-handler_102e.68k"

/* UI struct */
struct AppUI {
    Object *app;
    Object *win;
    Object *reg;

    Object *cycleVariant;
    Object *btnApplyBin;

    Object *keyStatus; /* keyfile presence */

    Object *gdClient, *gdSave, *gdLoad, *gdPurge, *gdMount, *gdUnmount, *gdStatus;
    Object *dbClient, *dbSave, *dbLoad, *dbPurge, *dbMount, *dbUnmount, *dbStatus;

    Object *logList;
    Object *logView;

    Object *grpGeneral;
    Object *grpGoogle;
    Object *grpDropbox;
};

/* Prototypes */
static STRPTR DupString(const char *s);
static void   LogAppend(const char *s);
static void   LogLineFlush(void);
static void   ExecCommand(const char *cmd);
static LONG   ReadSmallFile(const char *path, char *buf, LONG maxlen);
static LONG   WriteSmallFile(const char *path, const char *buf);
static void   EnsureDrawer(const char *fullpath);
static void   SaveToken(Object *stringobj, const char *filename);
static void   LoadTokenFile(const char *filename, Object *stringobj);
static void   LoadStringFromFile(Object *stringobj);
static void   DeleteIfExists(const char *p);
static void   PurgeTokenGoogle(Object *stringobj);
static void   PurgeTokenDropbox(Object *stringobj);
static void   DoMount(const char *volname);
static void   DoUnmount(const char *volname);
static void   UpdateMountlistVariant(ULONG variantIndex);
static ULONG  DetectMountlistVariant(void);
static void   UpdateTokenMountEnable(struct AppUI *ui);
static int    IsMounted(const char *assign);
static void   UpdateStatus(struct AppUI *ui);
static int    KeyfilePresent(void);
static void   UpdateKeyStatus(struct AppUI *ui);
static Object* MakeButton(const char *label);
static Object* MakeString(int maxlen);
static Object* MakeGroupGeneral(struct AppUI *ui);
static Object* MakeGroupGoogle(struct AppUI *ui);
static Object* MakeGroupDropbox(struct AppUI *ui);
static Object* BuildUI(struct AppUI *ui);

/* New: handler presence + tab disable */
static int FileExistsNoReq(const char *path);
static int HandlersPresentDropbox(void);
static int HandlersPresentGoogle(void);
static void UpdateHandlersAvailability(struct AppUI *ui);

/* Helpers */
static STRPTR DupString(const char *s) {
    char *p;
    size_t n;
    if (!s) return NULL;
    n = strlen(s);
    p = (char*)AllocVec(n+1, MEMF_ANY);
    if (p) { memcpy(p, s, n+1); }
    return (STRPTR)p;
}

/* Log as list */
static Object *g_LogList = NULL;
static char g_LogBuf[512];
static int  g_LogLen = 0;

static void LogLineFlush(void) {
    if (g_LogLen <= 0 || !g_LogList) return;
    g_LogBuf[g_LogLen] = 0;
    DoMethod(g_LogList, MUIM_List_InsertSingle, (ULONG)DupString(g_LogBuf), MUIV_List_Insert_Bottom);
    DoMethod(g_LogList, MUIM_List_Jump, MUIV_List_Jump_Bottom);
    g_LogLen = 0;
}

static void LogAppend(const char *s) {
    const unsigned char *p;
    if (!s) return;
    p = (const unsigned char*)s;
    while (*p) {
        unsigned char c = *p++;
        if (c == 10) {         /* LF */
            LogLineFlush();
        } else if (c == 13) {  /* CR */
        } else {
            if (g_LogLen < (int)sizeof(g_LogBuf)-1) g_LogBuf[g_LogLen++] = (char)c;
            else LogLineFlush();
        }
    }
}

static void ExecCommand(const char *cmd) {
    if (!cmd) return;
    if (!Execute((STRPTR)cmd, (BPTR)0, (BPTR)0)) LogAppend("[Exec] error\n");
    else LogAppend("[Exec] done\n");
}

static LONG ReadSmallFile(const char *path, char *buf, LONG maxlen) {
    BPTR fh;
    LONG n;
    fh = Open((STRPTR)path, MODE_OLDFILE);
    n = -1;
    if (fh) {
        n = Read(fh, buf, maxlen-1);
        if (n < 0) n = 0;
        buf[n] = 0;
        Close(fh);
    }
    return n;
}

static LONG WriteSmallFile(const char *path, const char *buf) {
    BPTR fh;
    LONG ok;
    LONG n;
    fh = Open((STRPTR)path, MODE_NEWFILE);
    ok = 0;
    if (fh) {
        n = Write(fh, (APTR)buf, (LONG)strlen(buf));
        Close(fh);
        ok = (n >= 0);
    }
    return ok;
}

static void EnsureDrawer(const char *fullpath) {
    int i, last, len, n;
    char tmp[300];
    last = -1;
    len = (int)strlen(fullpath);
    for (i=0;i<len;i++) {
        char c = fullpath[i];
        if ((unsigned char)c==CH_SLASH || (unsigned char)c==CH_COLON) last = i;
    }
    if (last > 0) {
        n = (last+1 < (int)sizeof(tmp)-1) ? last+1 : (int)sizeof(tmp)-1;
        memcpy(tmp, fullpath, n);
        tmp[n] = 0;
        CreateDir((STRPTR)tmp);
    }
}

static void SaveToken(Object *stringobj, const char *filename) {
    STRPTR s;
    ULONG len;
    s = NULL;
    len = 0;
    GetAttr(MUIA_String_Contents, stringobj, (ULONG*)&s);
    if (s) len = (ULONG)strlen((char*)s);
    if (!s || len == 0) { LogAppend("[Token] empty, nothing to save\n"); return; }
    EnsureDrawer(filename);
    if (WriteSmallFile(filename, (char*)s)) LogAppend("[Token] saved\n");
    else LogAppend("[Token] ERROR: write\n");
}

static void LoadTokenFile(const char *filename, Object *stringobj) {
    char buf[512];
    LONG n;
    int i;
    n = ReadSmallFile(filename, buf, sizeof(buf));
    if (n > 0) {
        for (i=(int)strlen(buf)-1; i>=0 && is_ws_or_nl((unsigned char)buf[i]); --i) buf[i]=0;
        DoMethod(stringobj, MUIM_Set, MUIA_String_Contents, (ULONG)buf);
        LogAppend("[Token] loaded from default file\n");
    }
}

static void LoadStringFromFile(Object *stringobj) {
    struct FileRequester *fr;
    fr = NULL;
    if (!AslBase) { LogAppend("[Token] ERROR: asl.library not open\n"); return; }
    fr = AllocAslRequest(ASL_FileRequest, NULL);
    if (fr) {
        if (AslRequest(fr, NULL)) {
            char path[300];
            int l, i;
            BPTR fh;
            LONG n;
            char buf[512];
            path[0] = 0;
            if (fr->fr_Drawer) { strncpy(path, (STRPTR)fr->fr_Drawer, sizeof(path)-1); path[sizeof(path)-1] = 0; }
            l = (int)strlen(path);
            if (l>0 && (unsigned char)path[l-1] != CH_COLON && (unsigned char)path[l-1] != CH_SLASH)
                strncat(path, "/", sizeof(path)-strlen(path)-1);
            if (fr->fr_File) strncat(path, (STRPTR)fr->fr_File, sizeof(path)-strlen(path)-1);
            fh = Open((STRPTR)path, MODE_OLDFILE);
            if (fh) {
                n = Read(fh, buf, sizeof(buf)-1);
                if (n > 0) {
                    buf[n] = 0;
                    for (i=(int)strlen(buf)-1; i>=0 && is_ws_or_nl((unsigned char)buf[i]); --i) buf[i]=0;
                    DoMethod(stringobj, MUIM_Set, MUIA_String_Contents, (ULONG)buf);
                    LogAppend("[Token] loaded\n");
                } else LogAppend("[Token] ERROR: empty file\n");
                Close(fh);
            } else LogAppend("[Token] ERROR: open file\n");
        }
        FreeAslRequest(fr);
    } else LogAppend("[Token] ERROR: AllocAslRequest failed\n");
}

static void DeleteIfExists(const char *p) {
    BPTR lk;
    lk = Lock((STRPTR)p, ACCESS_READ);
    if (lk) { UnLock(lk); DeleteFile((STRPTR)p); }
}

static void PurgeTokenGoogle(Object *stringobj) {
    DeleteIfExists(PATH_GD_ACCESS_TOKEN);
    DeleteIfExists(PATH_GD_REFRESH_TOKEN);
    DoMethod(stringobj, MUIM_Set, MUIA_String_Contents, (ULONG)"");
    LogAppend("[Token] Google: purged\n");
}

static void PurgeTokenDropbox(Object *stringobj) {
    DeleteIfExists(PATH_DB_ACCESS_TOKEN);
    DoMethod(stringobj, MUIM_Set, MUIA_String_Contents, (ULONG)"");
    LogAppend("[Token] Dropbox: purged\n");
}

static void DoMount(const char *volname) {
    char cmd[256];
    if (!volname) return;
    if (strcmp(volname,"GOOGLE:")==0) {
        strcpy(cmd, "Mount GOOGLE: FROM " PATH_CLOUD_MOUNTLIST);
    } else if (strcmp(volname,"DBOX:")==0) {
        strcpy(cmd, "Mount DBOX: FROM " PATH_CLOUD_MOUNTLIST);
    } else return;
    LogAppend("[Mount] "); LogAppend(cmd); LogAppend("\n");
    ExecCommand(cmd);
}

static void DoUnmount(const char *volname) {
    char cmd[64];
    if (!volname) return;
    strcpy(cmd, "Assign ");
    strcat(cmd, volname);
    strcat(cmd, " remove");
    LogAppend("[Unmount] "); LogAppend(cmd); LogAppend("\n");
    ExecCommand(cmd);
}

/* Safe in-place replace of mountlist */
static void UpdateMountlistVariant(ULONG variantIndex) {
    const char *db;
    const char *gd;
    BPTR in, out;
    char line[256];

    const char *finalPath = PATH_CLOUD_MOUNTLIST;
    const char *tmpPath   = PATH_CLOUD_TMP; /* same directory */
    const char *bakPath   = PATH_CLOUD_BAK;

    if (variantIndex==0) { db = FN_DB_68K;  gd = FN_GD_68K;  }
    else                  { db = FN_DB_102E; gd = FN_GD_102E; }

    in = Open((STRPTR)finalPath, MODE_OLDFILE);
    if (!in) { LogAppend("[Mountlist] not found at Devs:Cloud/cloud.mountlist\n"); return; }

    out = Open((STRPTR)tmpPath, MODE_NEWFILE);
    if (!out) { Close(in); LogAppend("[Mountlist] ERROR: open tmp\n"); return; }

    while (FGets(in, line, sizeof(line))) {
        if (strstr(line, "Handler = Devs:Cloud/dropbox-handler")) {
            char outln[128]; outln[0]=0;
            strcpy(outln, "    Handler = Devs:Cloud/"); strcat(outln, db); strcat(outln, "\n");
            FWrite(out, (APTR)outln, (LONG)strlen(outln), 1);
        } else if (strstr(line, "Handler = Devs:Cloud/google-drive-handler")) {
            char outln[128]; outln[0]=0;
            strcpy(outln, "    Handler = Devs:Cloud/"); strcat(outln, gd); strcat(outln, "\n");
            FWrite(out, (APTR)outln, (LONG)strlen(outln), 1);
        } else {
            FWrite(out, (APTR)line, (LONG)strlen(line), 1);
        }
    }
    Close(in);
    Close(out);

    { BPTR lk = Lock((STRPTR)finalPath, ACCESS_READ); if (lk) { UnLock(lk); Rename((STRPTR)finalPath, (STRPTR)bakPath); } }

    if (!Rename((STRPTR)tmpPath, (STRPTR)finalPath)) {
        LONG err = IoErr();
        char msg[64];
        LogAppend("[Mountlist] ERROR: replace (IoErr=");
        sprintf(msg, "%ld", (long)err);
        LogAppend(msg);
        LogAppend(")\n");
        { BPTR lk = Lock((STRPTR)bakPath, ACCESS_READ); if (lk) { UnLock(lk); Rename((STRPTR)bakPath, (STRPTR)finalPath); } else LogAppend("[Mountlist] WARNING: no backup to restore\n"); }
        DeleteFile((STRPTR)tmpPath);
        return;
    }

    DeleteFile((STRPTR)bakPath);
    LogAppend("[Mountlist] variant applied\n");
}

/* Detect current variant from mountlist (0 = 68k, 1 = 102e) */
static ULONG DetectMountlistVariant(void) {
    BPTR in;
    char line[256];
    ULONG v = 0;
    in = Open(PATH_CLOUD_MOUNTLIST, MODE_OLDFILE);
    if (!in) return 0;
    while (FGets(in, line, sizeof(line))) {
        if (strstr(line, "Handler = Devs:Cloud/dropbox-handler_102e") ||
            strstr(line, "Handler = Devs:Cloud/google-drive-handler_102e")) { v = 1; break; }
    }
    Close(in);
    return v;
}

/* Disable Save/Mount when empty */
static void UpdateTokenMountEnable(struct AppUI *ui) {
    STRPTR s;
    ULONG len;
    s = NULL; len = 0;
    if (ui->gdClient) { GetAttr(MUIA_String_Contents, ui->gdClient, (ULONG*)&s); if (s) len = (ULONG)strlen((char*)s); }
    if (ui->gdSave)   DoMethod(ui->gdSave,  MUIM_Set, MUIA_Disabled, (ULONG)(len==0));
    if (ui->gdMount)  DoMethod(ui->gdMount, MUIM_Set, MUIA_Disabled, (ULONG)(len==0));

    s = NULL; len = 0;
    if (ui->dbClient) { GetAttr(MUIA_String_Contents, ui->dbClient, (ULONG*)&s); if (s) len = (ULONG)strlen((char*)s); }
    if (ui->dbSave)   DoMethod(ui->dbSave,  MUIM_Set, MUIA_Disabled, (ULONG)(len==0));
    if (ui->dbMount)  DoMethod(ui->dbMount, MUIM_Set, MUIA_Disabled, (ULONG)(len==0));
}

/* No-requester probe used only after user actions */
static int IsMounted(const char *assign) {
    struct Process *pr;
    APTR  oldwin;    /* APTR is the correct type for pr_WindowPtr */
    BPTR  lk;

    pr = (struct Process*)FindTask(NULL);
    oldwin = pr ? pr->pr_WindowPtr : (APTR)0;
    if (pr) pr->pr_WindowPtr = (APTR)-1;  /* disable requesters */

    lk = Lock((STRPTR)assign, ACCESS_READ);

    if (pr) pr->pr_WindowPtr = oldwin;    /* restore */

    if (lk) { UnLock(lk); return 1; }
    return 0;
}

static void UpdateStatus(struct AppUI *ui) {
    DoMethod(ui->gdStatus, MUIM_Set, MUIA_Text_Contents, (ULONG)(IsMounted("GOOGLE:")? "GOOGLE: mounted" : "GOOGLE: not mounted"));
    DoMethod(ui->dbStatus, MUIM_Set, MUIA_Text_Contents, (ULONG)(IsMounted("DBOX:")?   "DBOX: mounted"   : "DBOX: not mounted"));
}

/* Keyfile detection */
static int ends_with(const char *s, const char *suf){
    int ls, lf; if(!s||!suf) return 0; ls=(int)strlen(s); lf=(int)strlen(suf); if(lf>ls) return 0; return stricmp(s+ls-lf, suf)==0;
}
static void tolower_inplace(char *s){ int i; for(i=0;s[i];++i){ if(s[i]>='A'&&s[i]<='Z') s[i]+=32; } }
static int KeyfilePresent(void){
    struct Process *pr=(struct Process*)FindTask(NULL);
    APTR oldwin = pr? pr->pr_WindowPtr : (APTR)0;   /* APTR */
    BPTR lock;
    struct FileInfoBlock *fib;
    int found=0;

    if(pr) pr->pr_WindowPtr = (APTR)-1;            /* disable requesters */

    lock = Lock("Devs:Cloud", ACCESS_READ);
    if(!lock){
        if(pr) pr->pr_WindowPtr = oldwin;
        return 0;
    }
    fib = (struct FileInfoBlock*)AllocDosObject(DOS_FIB, NULL);
    if(fib){
        if(Examine(lock, fib)){
            while(ExNext(lock, fib)){
                if(fib->fib_DirEntryType < 0){
                    char name[108]; strncpy(name, (char*)fib->fib_FileName, sizeof(name)-1); name[sizeof(name)-1]=0;
                    tolower_inplace(name);
                    if(!strcmp(name,"keyfile") || ends_with(name,".key") || ends_with(name,".keyfile") || strstr(name,"keyfile")){
                        found=1; break;
                    }
                }
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    if(pr) pr->pr_WindowPtr = oldwin;              /* restore */
    return found;
}
static void UpdateKeyStatus(struct AppUI *ui){
    DoMethod(ui->keyStatus, MUIM_Set, MUIA_Text_Contents,
        (ULONG)(KeyfilePresent()? "Registered (read-write)":"Unregistered (read-only)"));
}

/* File exists without requesters */
static int FileExistsNoReq(const char *path){
    struct Process *pr=(struct Process*)FindTask(NULL);
    APTR oldwin = pr? pr->pr_WindowPtr : (APTR)0;   /* APTR */
    BPTR lk;
    int ok=0;
    if(pr) pr->pr_WindowPtr = (APTR)-1;            /* disable requesters */
    lk = Lock((STRPTR)path, ACCESS_READ);
    if(lk){ UnLock(lk); ok=1; }
    if(pr) pr->pr_WindowPtr = oldwin;              /* restore */
    return ok;
}

static int HandlersPresentDropbox(void){
    return FileExistsNoReq("Devs:Cloud/" FN_DB_68K) || FileExistsNoReq("Devs:Cloud/" FN_DB_102E);
}
static int HandlersPresentGoogle(void){
    return FileExistsNoReq("Devs:Cloud/" FN_GD_68K) || FileExistsNoReq("Devs:Cloud/" FN_GD_102E);
}

static void UpdateHandlersAvailability(struct AppUI *ui){
    int hasDB = HandlersPresentDropbox();
    int hasGD = HandlersPresentGoogle();

    if(ui->grpDropbox){
        DoMethod(ui->grpDropbox, MUIM_Set, MUIA_Disabled, hasDB? FALSE:TRUE);
        if(!hasDB) LogAppend("[Dropbox] handlers missing; tab disabled\n");
    }
    if(ui->grpGoogle){
        DoMethod(ui->grpGoogle, MUIM_Set, MUIA_Disabled, hasGD? FALSE:TRUE);
        if(!hasGD) LogAppend("[Google] handlers missing; tab disabled\n");
    }
}

/* UI */
static Object* MakeButton(const char *label) { return MUI_MakeObject(MUIO_Button, (ULONG)label); }
static Object* MakeString(int maxlen)        { return MUI_MakeObject(MUIO_String, NULL, maxlen); }

static Object* MakeGroupGeneral(struct AppUI *ui) {
    static const STRPTR titles[] = { "68020", "68060/80", NULL };
    Object *col;
    col = MUI_NewObject(MUIC_Group,
            MUIA_Frame, MUIV_Frame_Group,
            MUIA_Group_Columns, 2,
            Child, MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"Binary:", TAG_DONE),
            Child, (ui->cycleVariant = MUI_NewObject(MUIC_Cycle, MUIA_Cycle_Active, 0, MUIA_Cycle_Entries, (ULONG)titles, TAG_DONE)),
            Child, MUI_NewObject(MUIC_Rectangle, TAG_DONE),
            Child, (ui->btnApplyBin = MakeButton("Apply")),
            Child, MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"Registration:", TAG_DONE),
            Child, (ui->keyStatus = MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"(checking...)", TAG_DONE)),
            TAG_DONE);
    return col;
}

static Object* MakeGroupGoogle(struct AppUI *ui) {
    Object *grp;
    grp = MUI_NewObject(MUIC_Group,
            MUIA_Frame, MUIV_Frame_Group,
            MUIA_Group_Spacing, 4,
            MUIA_Group_Columns, 3,
            Child, (ui->gdClient = MakeString(256)),
            Child, (ui->gdSave   = MakeButton("Save")),
            Child, (ui->gdLoad   = MakeButton("Load...")),
            Child, (ui->gdPurge  = MakeButton("Purge")),
            Child, (ui->gdMount  = MakeButton("Mount")),
            Child, (ui->gdUnmount= MakeButton("Unmount")),
            Child, (ui->gdStatus = MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"GOOGLE: status unknown", TAG_DONE)),
            TAG_DONE);

    DoMethod(ui->gdClient, MUIM_Set, MUIA_FixWidthTxt, (ULONG)"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    return grp;
}

static Object* MakeGroupDropbox(struct AppUI *ui) {
    Object *grp;
    grp = MUI_NewObject(MUIC_Group,
            MUIA_Frame, MUIV_Frame_Group,
            MUIA_Group_Spacing, 4,
            MUIA_Group_Columns, 3,
            Child, (ui->dbClient = MakeString(256)),
            Child, (ui->dbSave   = MakeButton("Save")),
            Child, (ui->dbLoad   = MakeButton("Load...")),
            Child, (ui->dbPurge  = MakeButton("Purge")),
            Child, (ui->dbMount  = MakeButton("Mount")),
            Child, (ui->dbUnmount= MakeButton("Unmount")),
            Child, (ui->dbStatus = MUI_NewObject(MUIC_Text, MUIA_Text_Contents, (ULONG)"DBOX: status unknown", TAG_DONE)),
            TAG_DONE);

    DoMethod(ui->dbClient, MUIM_Set, MUIA_FixWidthTxt, (ULONG)"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    return grp;
}

static Object* BuildUI(struct AppUI *ui) {
    static STRPTR tabs[] = { "General", "Google Drive", "Dropbox", NULL };

    ui->grpGeneral = MakeGroupGeneral(ui);
    ui->grpGoogle  = MakeGroupGoogle(ui);
    ui->grpDropbox = MakeGroupDropbox(ui);

    ui->logList = MUI_NewObject(MUIC_List, TAG_DONE);
    g_LogList = ui->logList;
    ui->logView = MUI_NewObject(MUIC_Listview,
        MUIA_Listview_List, (ULONG)ui->logList,
        MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_None,
        TAG_DONE);

    ui->reg = MUI_NewObject(MUIC_Register,
        MUIA_Register_Titles, (ULONG)tabs,
        Child, ui->grpGeneral,
        Child, ui->grpGoogle,
        Child, ui->grpDropbox,
        TAG_DONE);

    ui->win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title,      (ULONG)"AmigaCloudConfig",
        MUIA_Window_SizeGadget, TRUE,
        MUIA_Window_DepthGadget,TRUE,
        MUIA_Window_DragBar,    TRUE,
        MUIA_Window_CloseGadget,TRUE,
        WindowContents, MUI_NewObject(MUIC_Group,
            MUIA_Group_Spacing, 4,
            Child, ui->reg,
            Child, MUI_NewObject(MUIC_Rectangle, MUIA_Rectangle_HBar, TRUE, TAG_DONE),
            Child, ui->logView,
            TAG_DONE),
        TAG_DONE);

    ui->app = ApplicationObject,
        MUIA_Application_Title, (ULONG)"AmigaCloudConfig",
        SubWindow, ui->win,
    End;

    return ui->app;
}

/* main */
int main(void) {
    struct AppUI ui;
    ULONG sigs;
    ULONG ret;

    memset(&ui, 0, sizeof(ui));
    sigs = 0;

    MUIMasterBase = OpenLibrary("muimaster.library", 0);
    AslBase       = OpenLibrary("asl.library", 37);
    if (!MUIMasterBase) return 20;

    if (!BuildUI(&ui)) return 20;

    { ULONG v = DetectMountlistVariant(); DoMethod(ui.cycleVariant, MUIM_Set, MUIA_Cycle_Active, v); }

    UpdateHandlersAvailability(&ui);

    DoMethod(ui.gdSave,   MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_GD_SAVE);
    DoMethod(ui.gdLoad,   MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_GD_LOAD);
    DoMethod(ui.gdPurge,  MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_GD_PURGE);
    DoMethod(ui.gdMount,  MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_GD_MNT);
    DoMethod(ui.gdUnmount,MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_GD_UMNT);

    DoMethod(ui.dbSave,   MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_DB_SAVE);
    DoMethod(ui.dbLoad,   MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_DB_LOAD);
    DoMethod(ui.dbPurge,  MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_DB_PURGE);
    DoMethod(ui.dbMount,  MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_DB_MNT);
    DoMethod(ui.dbUnmount,MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_DB_UMNT);

    DoMethod(ui.btnApplyBin, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_APPLY_BIN);

    DoMethod(ui.gdClient, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_UPDATE_BTNS);
    DoMethod(ui.dbClient, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (ULONG)ui.app, 2, MUIM_Application_ReturnID, ID_UPDATE_BTNS);

    DoMethod(ui.win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)ui.app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(ui.win, MUIM_Set, MUIA_Window_Open, TRUE);
    LoadTokenFile(PATH_GD_CLIENT_CODE, ui.gdClient);
    LoadTokenFile(PATH_DB_CLIENT_CODE, ui.dbClient);
    UpdateTokenMountEnable(&ui);
    UpdateKeyStatus(&ui);

    for (;;) {
        ret = DoMethod(ui.app, MUIM_Application_NewInput, (ULONG)&sigs);
        if (ret == MUIV_Application_ReturnID_Quit) break;

        switch (ret) {
            case ID_UPDATE_BTNS:
                UpdateTokenMountEnable(&ui);
                break;
            case ID_GD_SAVE:  SaveToken(ui.gdClient, PATH_GD_CLIENT_CODE); break;
            case ID_DB_SAVE:  SaveToken(ui.dbClient, PATH_DB_CLIENT_CODE); break;
            case ID_GD_LOAD:  LoadStringFromFile(ui.gdClient); UpdateTokenMountEnable(&ui); break;
            case ID_DB_LOAD:  LoadStringFromFile(ui.dbClient); UpdateTokenMountEnable(&ui); break;
            case ID_GD_PURGE: PurgeTokenGoogle(ui.gdClient);   UpdateTokenMountEnable(&ui); break;
            case ID_DB_PURGE: PurgeTokenDropbox(ui.dbClient);  UpdateTokenMountEnable(&ui); break;
            case ID_GD_MNT:   DoMount("GOOGLE:"); UpdateStatus(&ui); break;
            case ID_DB_MNT:   DoMount("DBOX:");   UpdateStatus(&ui); break;
            case ID_GD_UMNT:  DoUnmount("GOOGLE:"); UpdateStatus(&ui); break;
            case ID_DB_UMNT:  DoUnmount("DBOX:");   UpdateStatus(&ui); break;
            case ID_APPLY_BIN: {
                ULONG act = 0;
                GetAttr(MUIA_Cycle_Active, ui.cycleVariant, (ULONG*)&act);
                UpdateMountlistVariant(act);
                UpdateStatus(&ui);
                break; }
        }
        if (ret == 0) { if (sigs) Wait(sigs); }
    }

    MUI_DisposeObject(ui.app);
    if (AslBase) CloseLibrary(AslBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    return 0;
}
