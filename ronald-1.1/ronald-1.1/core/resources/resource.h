#pragma once

// Menu / accelerator commands
#define IDM_FILE_OPEN       40001
#define IDM_FILE_SAVE       40002
#define IDM_FILE_SAVEAS     40003
#define IDM_FILE_EXIT       40004
#define IDM_EDIT_UNDO       40005
#define IDM_EDIT_REDO       40006
#define IDM_EDIT_FIND       40007
#define IDM_HELP_ABOUT      40008
#define IDM_FILE_CLOSE      40009
#define IDM_EDIT_SELECTALL  40010
#define IDM_EDIT_COPY       40011
#define IDM_EDIT_PASTE      40012
#define IDM_FILE_NEXTTAB    40013
#define IDM_FILE_PREVTAB    40014
#define IDM_FILE_SAVEALL    40015
#define IDM_EDIT_DELETE              40016
#define IDM_TOOLS_WIPE_EDITOR_TEMPS  40017
#define IDM_TOOLS_WIPE_FREE_SPACE    40018
#define IDM_TOOLS_DUMP_FOLDER_RAW    40019

#define IDR_MAINMENU        101
#define IDR_ACCEL           102
#define IDR_MANIFEST        1

// Toolbar button command IDs (shared with menu IDs above so one handler
// in MainWindow serves both the menu and the toolbar).
#define IDC_STATUSBAR       2001
#define IDC_TOOLBAR         2002
#define IDC_HEXGRID         2003
#define IDC_TABBAR          2004

// Find dialog
#define IDD_FIND            3000
#define IDC_FIND_MODE_HEX   3001
#define IDC_FIND_MODE_TEXT  3002
#define IDC_FIND_PATTERN    3003
#define IDC_FIND_CASESENS   3004
#define IDC_FIND_NEXT       3005
#define IDC_FIND_PREV       3006
#define IDC_FIND_CLOSE      3007
#define IDC_FIND_STATUS     3008
