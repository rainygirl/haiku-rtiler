/*
 * R Tiler -- a Deskbar tray item that lays the open windows out on screen.
 *
 * Built for a 1600x768 panel: wide enough for two or three full-height
 * columns, too short to stack much vertically, so the layouts prefer columns
 * and only add a second row once there are more windows than fit across.
 *
 * Other applications' windows are moved through the public scripting
 * protocol -- BWindow exposes Frame, Look, Hidden, Minimize and Workspaces as
 * scriptable properties, so this needs no private headers and nothing that
 * breaks between releases. It is the same mechanism `hey` uses.
 *
 * The tray item lives inside Deskbar's own process, so a crash here takes the
 * Deskbar down with it: every message out has a timeout, and the scan runs on
 * its own thread so an unresponsive application cannot freeze the tray.
 *
 * Distributed under the terms of the MIT License.
 */

#include <Application.h>
#include <Autolock.h>
#include <Deskbar.h>
#include <List.h>
#include <Locker.h>
#include <LocaleRoster.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <PropertyInfo.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char* const kAppSignature = "application/x-vnd.RTiler";
static const char* const kDeskbarItemName = "RTiler";
static const char* const kViewClassName = "RTilerView";

// Deskbar must not be left waiting on an application that is busy or wedged.
static const bigtime_t kSendTimeout = 200000;
static const bigtime_t kReplyTimeout = 400000;

// Half the gap between two neighbouring windows, and the room the decorator's
// tab needs above the content rect.
//
// Both are applied to the *content* rectangle, which is all the scripting
// protocol exposes -- the decorator draws its border a few pixels outside it
// on every side. A 5 px inset therefore looks like barely 2 px of daylight
// once two borders have eaten into the 10 px gap it produces, so the inset is
// set well above the gap actually wanted. Neither the border width nor the
// tab height has a public API; these are measured against the default
// decorator at the default font size.
static const float kMargin		= 5.0f;
static const float kTabHeight	= 26.0f;

static const uint32 kMsgLayoutAuto		= 'lyat';
static const uint32 kMsgLayoutTwo		= 'ly02';
static const uint32 kMsgLayoutThree		= 'ly03';
static const uint32 kMsgLayoutGrid		= 'lygr';
static const uint32 kMsgLayoutMaximize	= 'lymx';
static const uint32 kMsgRestore			= 'lyrs';
static const uint32 kMsgQuit			= 'lyqt';


// #pragma mark - strings


enum string_id {
	kStrAuto = 0, kStrTwo, kStrThree, kStrGrid, kStrMaximize, kStrRestore,
	kStrQuit, kStrTip,
	kStringCount
};

static const char* const kStringsEn[kStringCount] = {
	"Tile automatically", "Two columns", "Three columns", "Grid",
	"Maximize all", "Undo", "Remove from Deskbar", "Tile windows"
};

static const char* const kStringsKo[kStringCount] = {
	"자동 배치", "2단", "3단", "격자",
	"모두 최대화", "되돌리기", "Deskbar에서 제거", "창 배치"
};

static const char* const kStringsJa[kStringCount] = {
	"自動整列", "2列", "3列", "グリッド",
	"すべて最大化", "元に戻す", "Deskbar から削除", "ウィンドウを整列"
};

static const char* const kStringsDe[kStringCount] = {
	"Automatisch anordnen", "Zwei Spalten", "Drei Spalten", "Raster",
	"Alle maximieren", "Rückgängig", "Aus dem Deskbar entfernen",
	"Fenster anordnen"
};

static const char* const kStringsFr[kStringCount] = {
	"Disposer automatiquement", "Deux colonnes", "Trois colonnes", "Grille",
	"Tout agrandir", "Annuler", "Retirer du Deskbar", "Disposer les fenêtres"
};

static const char* const* sStrings = kStringsEn;


static inline const char*
T(string_id id)
{
	return sStrings[id];
}


static void
choose_language()
{
	BMessage preferred;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&preferred) != B_OK)
		return;

	const char* language = NULL;
	for (int32 i = 0;
			preferred.FindString("language", i, &language) == B_OK; i++) {
		if (language == NULL)
			continue;
		if (strncmp(language, "ko", 2) == 0)
			sStrings = kStringsKo;
		else if (strncmp(language, "ja", 2) == 0)
			sStrings = kStringsJa;
		else if (strncmp(language, "de", 2) == 0)
			sStrings = kStringsDe;
		else if (strncmp(language, "fr", 2) == 0)
			sStrings = kStringsFr;
		else
			continue;
		return;
	}
}


// #pragma mark - window discovery


struct window_ref {
	team_id	team;
	int32	index;
	BRect	frame;
};


// One scripting round trip. Returns false on timeout as readily as on error:
// a window that cannot answer is a window this tool leaves alone.
static bool
get_window_property(const BMessenger& messenger, int32 index,
	const char* property, BMessage* reply)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier(property);
	request.AddSpecifier("Window", index);

	if (messenger.SendMessage(&request, reply, kSendTimeout, kReplyTimeout)
			!= B_OK) {
		return false;
	}

	status_t error = B_OK;
	if (reply->FindInt32("error", &error) == B_OK && error != B_OK)
		return false;

	return true;
}


static bool
set_window_frame(const BMessenger& messenger, int32 index, BRect frame)
{
	BMessage request(B_SET_PROPERTY);
	request.AddRect("data", frame);
	request.AddSpecifier("Frame");
	request.AddSpecifier("Window", index);

	BMessage reply;
	return messenger.SendMessage(&request, &reply, kSendTimeout, kReplyTimeout)
		== B_OK;
}


// Only ordinary application windows are moved. Panels, menus, the Desktop and
// anything borderless (R Memo's notes, for instance) keep their place, which
// is what makes clicking this safe with a full screen of mixed windows.
static bool
is_tileable(const BMessenger& messenger, int32 index, uint32 workspace,
	BRect* frame)
{
	BMessage reply;

	if (!get_window_property(messenger, index, "Look", &reply))
		return false;
	int32 look = 0;
	if (reply.FindInt32("result", &look) != B_OK)
		return false;
	if (look != B_TITLED_WINDOW_LOOK && look != B_DOCUMENT_WINDOW_LOOK)
		return false;

	reply.MakeEmpty();
	if (get_window_property(messenger, index, "Hidden", &reply)) {
		bool hidden = false;
		if (reply.FindBool("result", &hidden) == B_OK && hidden)
			return false;
	}

	reply.MakeEmpty();
	if (get_window_property(messenger, index, "Minimize", &reply)) {
		bool minimized = false;
		if (reply.FindBool("result", &minimized) == B_OK && minimized)
			return false;
	}

	reply.MakeEmpty();
	if (get_window_property(messenger, index, "Workspaces", &reply)) {
		int32 workspaces = 0;
		if (reply.FindInt32("result", &workspaces) == B_OK
			&& (workspaces & workspace) == 0) {
			return false;
		}
	}

	reply.MakeEmpty();
	if (!get_window_property(messenger, index, "Frame", &reply))
		return false;

	return reply.FindRect("result", frame) == B_OK;
}


static bool
is_skipped_signature(const char* signature)
{
	if (signature == NULL || signature[0] == '\0')
		return true;

	// Deskbar hosts this code; asking it to move itself is neither useful nor
	// safe. Everything else, Tracker included, is fair game -- Tracker's
	// Desktop window is borderless and drops out on the Look test above.
	static const char* const kSkip[] = {
		"application/x-vnd.Be-TSKB",
		"application/x-vnd.Be-TSKB ",
		"application/x-vnd.Haiku-screen_blanker",
		NULL
	};

	for (int32 i = 0; kSkip[i] != NULL; i++) {
		if (strcasecmp(signature, kSkip[i]) == 0)
			return true;
	}

	return false;
}


static void
collect_windows(BList* into)
{
	BList teams;
	be_roster->GetAppList(&teams);

	uint32 workspace = 1UL << current_workspace();

	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);

		app_info info;
		if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
			continue;
		if (is_skipped_signature(info.signature))
			continue;

		BMessenger messenger(NULL, team);
		if (!messenger.IsValid())
			continue;

		BMessage request(B_COUNT_PROPERTIES);
		request.AddSpecifier("Window");
		BMessage reply;
		if (messenger.SendMessage(&request, &reply, kSendTimeout,
				kReplyTimeout) != B_OK) {
			continue;
		}

		int32 count = 0;
		if (reply.FindInt32("result", &count) != B_OK)
			continue;

		for (int32 w = 0; w < count; w++) {
			BRect frame;
			if (!is_tileable(messenger, w, workspace, &frame))
				continue;

			window_ref* ref = new window_ref;
			ref->team = team;
			ref->index = w;
			ref->frame = frame;
			into->AddItem(ref);
		}
	}
}


// The area a window may occupy: the screen minus whatever edge Deskbar is
// docked to. Tiling over the tray would hide the very control that did it.
static BRect
work_area()
{
	BScreen screen;
	BRect area = screen.Frame();

	BDeskbar deskbar;
	BRect bar = deskbar.Frame();
	if (deskbar.IsAutoHide() || !bar.IsValid())
		return area;

	switch (deskbar.Location()) {
		case B_DESKBAR_TOP:
			area.top = bar.bottom + 1;
			break;
		case B_DESKBAR_BOTTOM:
			area.bottom = bar.top - 1;
			break;
		case B_DESKBAR_LEFT_TOP:
		case B_DESKBAR_LEFT_BOTTOM:
			area.left = bar.right + 1;
			break;
		case B_DESKBAR_RIGHT_TOP:
		case B_DESKBAR_RIGHT_BOTTOM:
			area.right = bar.left - 1;
			break;
		default:
			break;
	}

	return area;
}


// #pragma mark - layout


static void
grid_layout(BList* windows, int32 columns, int32 rows)
{
	if (columns < 1 || rows < 1)
		return;

	BRect area = work_area();
	float width = (area.Width() + 1) / columns;
	float height = (area.Height() + 1) / rows;

	for (int32 i = 0; i < windows->CountItems(); i++) {
		window_ref* ref = (window_ref*)windows->ItemAt(i);

		int32 cell = i % (columns * rows);
		int32 column = cell % columns;
		int32 row = cell / columns;

		BRect frame(area.left + column * width, area.top + row * height, 0, 0);
		frame.right = frame.left + width - 1;
		frame.bottom = frame.top + height - 1;

		frame.InsetBy(kMargin, kMargin);
		// Frame is the *content* rect -- the decorator's tab is drawn above
		// it. Without this the tab of every window in the top row would sit
		// off the top of its cell, and for the first row, off the screen.
		frame.top += kTabHeight;

		BMessenger messenger(NULL, ref->team);
		set_window_frame(messenger, ref->index, frame);
	}
}


// Columns before rows: this panel is 1600 wide and only 768 tall, so a second
// row costs far more than a narrower column does.
static void
auto_layout(BList* windows)
{
	int32 count = windows->CountItems();
	if (count <= 0)
		return;

	if (count <= 3) {
		grid_layout(windows, count, 1);
		return;
	}
	if (count == 4) {
		grid_layout(windows, 2, 2);
		return;
	}

	int32 columns = (count + 1) / 2;
	if (columns > 4)
		columns = 4;
	grid_layout(windows, columns, 2);
}


// #pragma mark - RTilerView


class RTilerView : public BView {
public:
							RTilerView(BRect frame);
							RTilerView(BMessage* archive);
	virtual					~RTilerView();

	static	RTilerView*		Instantiate(BMessage* archive);
	virtual	status_t		Archive(BMessage* archive, bool deep = true) const;

	virtual	void			AttachedToWindow();
	virtual	void			Draw(BRect updateRect);
	virtual	void			MouseDown(BPoint where);
	virtual	void			MessageReceived(BMessage* message);

private:
	static	int32			_WorkEntry(void* data);
			void			_Work(uint32 what);
			void			_Start(uint32 what);
			void			_SaveFrames(BList* windows);
			void			_Restore();
			void			_FreeWindows(BList* windows);

			BLocker			fLock;
			BList			fSaved;
			uint32			fPending;
			bool			fBusy;
};


RTilerView::RTilerView(BRect frame)
	:
	BView(frame, kDeskbarItemName, B_FOLLOW_NONE, B_WILL_DRAW),
	fLock("tiler"),
	fPending(0),
	fBusy(false)
{
}


RTilerView::RTilerView(BMessage* archive)
	:
	BView(archive),
	fLock("tiler"),
	fPending(0),
	fBusy(false)
{
}


RTilerView::~RTilerView()
{
	_FreeWindows(&fSaved);
}


RTilerView*
RTilerView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, kViewClassName))
		return NULL;

	return new RTilerView(archive);
}


status_t
RTilerView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", kAppSignature);
	if (status == B_OK)
		status = archive->AddString("class", kViewClassName);

	return status;
}


void
RTilerView::AttachedToWindow()
{
	BView::AttachedToWindow();
	SetViewColor(B_TRANSPARENT_COLOR);
	choose_language();
	SetToolTip(T(kStrTip));
}


void
RTilerView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();

	rgb_color background = Parent() != NULL
		? Parent()->ViewColor() : ui_color(B_PANEL_BACKGROUND_COLOR);
	SetHighColor(background);
	FillRect(bounds);

	// Three panes butted together: what the tool does, at a glance.
	BRect box = bounds;
	box.InsetBy(1, 3);

	rgb_color line = tint_color(background, B_DARKEN_4_TINT);
	rgb_color fill = tint_color(background, B_DARKEN_2_TINT);

	float width = (box.Width() + 1) / 3;
	for (int32 i = 0; i < 3; i++) {
		BRect pane(box.left + i * width, box.top,
			box.left + (i + 1) * width - 1, box.bottom);

		// The middle one is filled so the three read as separate panes even
		// at 16 pixels, where three outlines alone blur into a single box.
		SetHighColor(i == 1 ? fill : background);
		FillRect(pane);
		SetHighColor(line);
		StrokeRect(pane);
	}
}


void
RTilerView::MouseDown(BPoint where)
{
	int32 buttons = B_PRIMARY_MOUSE_BUTTON;
	BMessage* current = Window() != NULL ? Window()->CurrentMessage() : NULL;
	if (current != NULL)
		current->FindInt32("buttons", &buttons);

	if ((buttons & B_PRIMARY_MOUSE_BUTTON) != 0) {
		_Start(kMsgLayoutAuto);
		return;
	}

	BPopUpMenu* menu = new BPopUpMenu("tiler", false, false);
	menu->AddItem(new BMenuItem(T(kStrAuto), new BMessage(kMsgLayoutAuto)));
	menu->AddItem(new BMenuItem(T(kStrTwo), new BMessage(kMsgLayoutTwo)));
	menu->AddItem(new BMenuItem(T(kStrThree), new BMessage(kMsgLayoutThree)));
	menu->AddItem(new BMenuItem(T(kStrGrid), new BMessage(kMsgLayoutGrid)));
	menu->AddItem(new BMenuItem(T(kStrMaximize),
		new BMessage(kMsgLayoutMaximize)));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem(T(kStrRestore), new BMessage(kMsgRestore)));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem(T(kStrQuit), new BMessage(kMsgQuit)));

	BMenuItem* chosen = menu->Go(ConvertToScreen(where), false, true);
	if (chosen != NULL && chosen->Message() != NULL)
		_Start(chosen->Message()->what);

	delete menu;
}


void
RTilerView::MessageReceived(BMessage* message)
{
	BView::MessageReceived(message);
}


void
RTilerView::_Start(uint32 what)
{
	if (what == kMsgQuit) {
		BDeskbar deskbar;
		deskbar.RemoveItem(kDeskbarItemName);
		return;
	}

	{
		BAutolock lock(fLock);
		// One pass at a time. A second click while the first scan is still
		// walking the applications would interleave two sets of moves.
		if (fBusy)
			return;
		fBusy = true;
		fPending = what;
	}

	// Off the Deskbar window thread: the scan makes several round trips per
	// window, and doing that inline freezes the whole tray while it runs.
	thread_id thread = spawn_thread(_WorkEntry, "tiler work",
		B_NORMAL_PRIORITY, this);
	if (thread < B_OK) {
		BAutolock lock(fLock);
		fBusy = false;
		return;
	}

	resume_thread(thread);
}


int32
RTilerView::_WorkEntry(void* data)
{
	RTilerView* view = (RTilerView*)data;

	uint32 what;
	{
		BAutolock lock(view->fLock);
		what = view->fPending;
	}

	view->_Work(what);

	BAutolock lock(view->fLock);
	view->fBusy = false;
	return 0;
}


void
RTilerView::_Work(uint32 what)
{
	if (what == kMsgRestore) {
		_Restore();
		return;
	}

	BList windows;
	collect_windows(&windows);

	if (windows.CountItems() == 0) {
		_FreeWindows(&windows);
		return;
	}

	_SaveFrames(&windows);

	switch (what) {
		case kMsgLayoutTwo:
			grid_layout(&windows, 2, 1);
			break;
		case kMsgLayoutThree:
			grid_layout(&windows, 3, 1);
			break;
		case kMsgLayoutGrid:
			grid_layout(&windows, 2, 2);
			break;
		case kMsgLayoutMaximize:
			grid_layout(&windows, 1, 1);
			break;
		case kMsgLayoutAuto:
		default:
			auto_layout(&windows);
			break;
	}

	_FreeWindows(&windows);
}


void
RTilerView::_SaveFrames(BList* windows)
{
	BAutolock lock(fLock);
	_FreeWindows(&fSaved);

	for (int32 i = 0; i < windows->CountItems(); i++) {
		window_ref* source = (window_ref*)windows->ItemAt(i);
		window_ref* copy = new window_ref;
		*copy = *source;
		fSaved.AddItem(copy);
	}
}


void
RTilerView::_Restore()
{
	BList saved;
	{
		BAutolock lock(fLock);
		for (int32 i = 0; i < fSaved.CountItems(); i++) {
			window_ref* source = (window_ref*)fSaved.ItemAt(i);
			window_ref* copy = new window_ref;
			*copy = *source;
			saved.AddItem(copy);
		}
	}

	for (int32 i = 0; i < saved.CountItems(); i++) {
		window_ref* ref = (window_ref*)saved.ItemAt(i);
		BMessenger messenger(NULL, ref->team);
		if (messenger.IsValid())
			set_window_frame(messenger, ref->index, ref->frame);
	}

	_FreeWindows(&saved);
}


void
RTilerView::_FreeWindows(BList* windows)
{
	for (int32 i = 0; i < windows->CountItems(); i++)
		delete (window_ref*)windows->ItemAt(i);
	windows->MakeEmpty();
}


// #pragma mark -


extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	float size = maxHeight < 16 ? maxHeight : 16;
	return new RTilerView(BRect(0, 0, size, maxHeight - 1));
}


int
main(int argc, char** argv)
{
	BApplication app(kAppSignature);
	choose_language();

	// Also usable straight from the shell, which is how the scripting path
	// gets exercised without a tray click -- and it makes the layouts
	// available to a keyboard shortcut.
	if (argc > 1 && strncmp(argv[1], "--tile", 6) == 0) {
		BList windows;
		collect_windows(&windows);
		printf("tileable windows: %" B_PRId32 "\n", windows.CountItems());
		for (int32 i = 0; i < windows.CountItems(); i++) {
			window_ref* ref = (window_ref*)windows.ItemAt(i);
			app_info info;
			be_roster->GetRunningAppInfo(ref->team, &info);
			printf("  %-40s window %" B_PRId32 "\n", info.signature,
				ref->index);
		}

		const char* mode = argc > 2 ? argv[2] : "auto";
		if (strcmp(mode, "2") == 0)
			grid_layout(&windows, 2, 1);
		else if (strcmp(mode, "3") == 0)
			grid_layout(&windows, 3, 1);
		else if (strcmp(mode, "grid") == 0)
			grid_layout(&windows, 2, 2);
		else if (strcmp(mode, "max") == 0)
			grid_layout(&windows, 1, 1);
		else
			auto_layout(&windows);

		for (int32 i = 0; i < windows.CountItems(); i++)
			delete (window_ref*)windows.ItemAt(i);
		return 0;
	}

	BDeskbar deskbar;

	if (argc > 1 && strcmp(argv[1], "--remove") == 0) {
		if (deskbar.HasItem(kDeskbarItemName)) {
			deskbar.RemoveItem(kDeskbarItemName);
			printf("Removed from Deskbar.\n");
		} else
			printf("Not in the Deskbar.\n");
		return 0;
	}

	// Replacing rather than skipping, so reinstalling a new build actually
	// takes effect instead of leaving the old view in the tray.
	if (deskbar.HasItem(kDeskbarItemName))
		deskbar.RemoveItem(kDeskbarItemName);

	RTilerView* view = new RTilerView(BRect(0, 0, 15, 15));
	status_t status = deskbar.AddItem(view);
	if (status != B_OK) {
		printf("Could not add to the Deskbar: %s\n", strerror(status));
		delete view;
		return 1;
	}

	printf("Added to the Deskbar. Click the icon to tile, right-click for "
		"layouts.\n");
	return 0;
}
