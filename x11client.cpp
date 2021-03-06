/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
// own
#include "x11client.h"
// kwin
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "atoms.h"
#include "client_machine.h"
#include "composite.h"
#include "cursor.h"
#include "deleted.h"
#include "focuschain.h"
#include "group.h"
#include "netinfo.h"
#include "screens.h"
#include "shadow.h"
#ifdef KWIN_BUILD_TABBOX
#include "tabbox.h"
#endif
#include "workspace.h"
#include "screenedge.h"
#include "decorations/decorationbridge.h"
#include "decorations/decoratedclient.h"
#include <KDecoration2/Decoration>
#include <KDecoration2/DecoratedClient>
// KDE
#include <KLocalizedString>
#include <KStartupInfo>
#include <KWindowSystem>
#include <KColorScheme>
// Qt
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QProcess>
// xcb
#include <xcb/xcb_icccm.h>
// system
#include <unistd.h>
// c++
#include <csignal>

// Put all externs before the namespace statement to allow the linker
// to resolve them properly

namespace KWin
{

const long ClientWinMask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                           XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                           XCB_EVENT_MASK_KEYMAP_STATE |
                           XCB_EVENT_MASK_BUTTON_MOTION |
                           XCB_EVENT_MASK_POINTER_MOTION | // need this, too!
                           XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                           XCB_EVENT_MASK_FOCUS_CHANGE |
                           XCB_EVENT_MASK_EXPOSURE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

// window types that are supported as normal windows (i.e. KWin actually manages them)
const NET::WindowTypes SUPPORTED_MANAGED_WINDOW_TYPES_MASK = NET::NormalMask | NET::DesktopMask | NET::DockMask
        | NET::ToolbarMask | NET::MenuMask | NET::DialogMask /*| NET::OverrideMask*/ | NET::TopMenuMask
        | NET::UtilityMask | NET::SplashMask | NET::NotificationMask | NET::OnScreenDisplayMask
        | NET::CriticalNotificationMask;

// Creating a client:
//  - only by calling Workspace::createClient()
//      - it creates a new client and calls manage() for it
//
// Destroying a client:
//  - destroyClient() - only when the window itself has been destroyed
//      - releaseWindow() - the window is kept, only the client itself is destroyed

/**
 * \class Client x11client.h
 * \brief The Client class encapsulates a window decoration frame.
 */

/**
 * This ctor is "dumb" - it only initializes data. All the real initialization
 * is done in manage().
 */
X11Client::X11Client()
    : AbstractClient()
    , m_client()
    , m_wrapper()
    , m_frame()
    , m_activityUpdatesBlocked(false)
    , m_blockedActivityUpdatesRequireTransients(false)
    , m_moveResizeGrabWindow()
    , move_resize_has_keyboard_grab(false)
    , m_managed(false)
    , m_transientForId(XCB_WINDOW_NONE)
    , m_originalTransientForId(XCB_WINDOW_NONE)
    , shade_below(nullptr)
    , m_motif(atoms->motif_wm_hints)
    , blocks_compositing(false)
    , shadeHoverTimer(nullptr)
    , m_colormap(XCB_COLORMAP_NONE)
    , in_group(nullptr)
    , ping_timer(nullptr)
    , m_killHelperPID(0)
    , m_pingTimestamp(XCB_TIME_CURRENT_TIME)
    , m_userTime(XCB_TIME_CURRENT_TIME)   // Not known yet
    , allowed_actions()
    , shade_geometry_change(false)
    , sm_stacking_order(-1)
    , activitiesDefined(false)
    , sessionActivityOverride(false)
    , needsXWindowMove(false)
    , m_decoInputExtent()
    , m_focusOutTimer(nullptr)
{
    // TODO: Do all as initialization
    syncRequest.counter = syncRequest.alarm = XCB_NONE;
    syncRequest.timeout = syncRequest.failsafeTimeout = nullptr;
    syncRequest.lastTimestamp = xTime();
    syncRequest.isPending = false;

    // Set the initial mapping state
    mapping_state = Withdrawn;

    info = nullptr;

    shade_mode = ShadeNone;
    deleting = false;
    m_fullscreenMode = FullScreenNone;
    hidden = false;
    noborder = false;
    app_noborder = false;
    ignore_focus_stealing = false;
    check_active_modal = false;

    max_mode = MaximizeRestore;

    //Client to workspace connections require that each
    //client constructed be connected to the workspace wrapper

    m_frameGeometry = QRect(0, 0, 100, 100);   // So that decorations don't start with size being (0,0)

    connect(clientMachine(), &ClientMachine::localhostChanged, this, &X11Client::updateCaption);
    connect(options, &Options::condensedTitleChanged, this, &X11Client::updateCaption);

    connect(this, &X11Client::moveResizeCursorChanged, this, [this] (CursorShape cursor) {
        xcb_cursor_t nativeCursor = Cursor::x11Cursor(cursor);
        m_frame.defineCursor(nativeCursor);
        if (m_decoInputExtent.isValid())
            m_decoInputExtent.defineCursor(nativeCursor);
        if (isMoveResize()) {
            // changing window attributes doesn't change cursor if there's pointer grab active
            xcb_change_active_pointer_grab(connection(), nativeCursor, xTime(),
                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW);
        }
    });

    // SELI TODO: Initialize xsizehints??
}

/**
 * "Dumb" destructor.
 */
X11Client::~X11Client()
{
    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) { // means the process is alive
        ::kill(m_killHelperPID, SIGTERM);
        m_killHelperPID = 0;
    }
    if (syncRequest.alarm != XCB_NONE)
        xcb_sync_destroy_alarm(connection(), syncRequest.alarm);
    Q_ASSERT(!isMoveResize());
    Q_ASSERT(m_client == XCB_WINDOW_NONE);
    Q_ASSERT(m_wrapper == XCB_WINDOW_NONE);
    Q_ASSERT(m_frame == XCB_WINDOW_NONE);
    Q_ASSERT(!check_active_modal);
    for (auto it = m_connections.constBegin(); it != m_connections.constEnd(); ++it) {
        disconnect(*it);
    }
}

// Use destroyClient() or releaseWindow(), Client instances cannot be deleted directly
void X11Client::deleteClient(X11Client *c)
{
    delete c;
}

/**
 * Releases the window. The client has done its job and the window is still existing.
 */
void X11Client::releaseWindow(bool on_shutdown)
{
    Q_ASSERT(!deleting);
    deleting = true;
#ifdef KWIN_BUILD_TABBOX
    TabBox::TabBox *tabBox = TabBox::TabBox::self();
    if (tabBox->isDisplayed() && tabBox->currentClient() == this) {
        tabBox->nextPrev(true);
    }
#endif
    destroyWindowManagementInterface();
    Deleted* del = nullptr;
    if (!on_shutdown) {
        del = Deleted::create(this);
    }
    if (isMoveResize())
        emit clientFinishUserMovedResized(this);
    emit windowClosed(this, del);
    finishCompositing();
    RuleBook::self()->discardUsed(this, true);   // Remove ForceTemporarily rules
    StackingUpdatesBlocker blocker(workspace());
    if (isMoveResize())
        leaveMoveResize();
    finishWindowRules();
    blockGeometryUpdates();
    if (isOnCurrentDesktop() && isShown(true))
        addWorkspaceRepaint(visibleRect());
    // Grab X during the release to make removing of properties, setting to withdrawn state
    // and repareting to root an atomic operation (https://lists.kde.org/?l=kde-devel&m=116448102901184&w=2)
    grabXServer();
    exportMappingState(XCB_ICCCM_WM_STATE_WITHDRAWN);
    setModal(false);   // Otherwise its mainwindow wouldn't get focus
    hidden = true; // So that it's not considered visible anymore (can't use hideClient(), it would set flags)
    if (!on_shutdown)
        workspace()->clientHidden(this);
    m_frame.unmap();  // Destroying decoration would cause ugly visual effect
    destroyDecoration();
    cleanGrouping();
    if (!on_shutdown) {
        workspace()->removeClient(this);
        // Only when the window is being unmapped, not when closing down KWin (NETWM sections 5.5,5.7)
        info->setDesktop(0);
        info->setState(NET::States(), info->state());  // Reset all state flags
    }
    xcb_connection_t *c = connection();
    m_client.deleteProperty(atoms->kde_net_wm_user_creation_time);
    m_client.deleteProperty(atoms->net_frame_extents);
    m_client.deleteProperty(atoms->kde_net_wm_frame_strut);
    m_client.reparent(rootWindow(), m_bufferGeometry.x(), m_bufferGeometry.y());
    xcb_change_save_set(c, XCB_SET_MODE_DELETE, m_client);
    m_client.selectInput(XCB_EVENT_MASK_NO_EVENT);
    if (on_shutdown)
        // Map the window, so it can be found after another WM is started
        m_client.map();
    // TODO: Preserve minimized, shaded etc. state?
    else // Make sure it's not mapped if the app unmapped it (#65279). The app
        // may do map+unmap before we initially map the window by calling rawShow() from manage().
        m_client.unmap();
    m_client.reset();
    m_wrapper.reset();
    m_frame.reset();
    unblockGeometryUpdates(); // Don't use GeometryUpdatesBlocker, it would now set the geometry
    if (!on_shutdown) {
        disownDataPassedToDeleted();
        del->unrefWindow();
    }
    deleteClient(this);
    ungrabXServer();
}

/**
 * Like releaseWindow(), but this one is called when the window has been already destroyed
 * (E.g. The application closed it)
 */
void X11Client::destroyClient()
{
    Q_ASSERT(!deleting);
    deleting = true;
#ifdef KWIN_BUILD_TABBOX
    TabBox::TabBox *tabBox = TabBox::TabBox::self();
    if (tabBox && tabBox->isDisplayed() && tabBox->currentClient() == this) {
        tabBox->nextPrev(true);
    }
#endif
    destroyWindowManagementInterface();
    Deleted* del = Deleted::create(this);
    if (isMoveResize())
        emit clientFinishUserMovedResized(this);
    emit windowClosed(this, del);
    finishCompositing(ReleaseReason::Destroyed);
    RuleBook::self()->discardUsed(this, true);   // Remove ForceTemporarily rules
    StackingUpdatesBlocker blocker(workspace());
    if (isMoveResize())
        leaveMoveResize();
    finishWindowRules();
    blockGeometryUpdates();
    if (isOnCurrentDesktop() && isShown(true))
        addWorkspaceRepaint(visibleRect());
    setModal(false);
    hidden = true; // So that it's not considered visible anymore
    workspace()->clientHidden(this);
    destroyDecoration();
    cleanGrouping();
    workspace()->removeClient(this);
    m_client.reset(); // invalidate
    m_wrapper.reset();
    m_frame.reset();
    unblockGeometryUpdates(); // Don't use GeometryUpdatesBlocker, it would now set the geometry
    disownDataPassedToDeleted();
    del->unrefWindow();
    deleteClient(this);
}

/**
 * Manages the clients. This means handling the very first maprequest:
 * reparenting, initial geometry, initial state, placement, etc.
 * Returns false if KWin is not going to manage this window.
 */
bool X11Client::manage(xcb_window_t w, bool isMapped)
{
    StackingUpdatesBlocker stacking_blocker(workspace());

    Xcb::WindowAttributes attr(w);
    Xcb::WindowGeometry windowGeometry(w);
    if (attr.isNull() || windowGeometry.isNull()) {
        return false;
    }

    // From this place on, manage() must not return false
    blockGeometryUpdates();
    setPendingGeometryUpdate(PendingGeometryForced); // Force update when finishing with geometry changes

    embedClient(w, attr->visual, attr->colormap, windowGeometry->depth);

    m_visual = attr->visual;
    bit_depth = windowGeometry->depth;

    // SELI TODO: Order all these things in some sane manner

    const NET::Properties properties =
        NET::WMDesktop |
        NET::WMState |
        NET::WMWindowType |
        NET::WMStrut |
        NET::WMName |
        NET::WMIconGeometry |
        NET::WMIcon |
        NET::WMPid |
        NET::WMIconName;
    const NET::Properties2 properties2 =
        NET::WM2BlockCompositing |
        NET::WM2WindowClass |
        NET::WM2WindowRole |
        NET::WM2UserTime |
        NET::WM2StartupId |
        NET::WM2ExtendedStrut |
        NET::WM2Opacity |
        NET::WM2FullscreenMonitors |
        NET::WM2FrameOverlap |
        NET::WM2GroupLeader |
        NET::WM2Urgency |
        NET::WM2Input |
        NET::WM2Protocols |
        NET::WM2InitialMappingState |
        NET::WM2IconPixmap |
        NET::WM2OpaqueRegion |
        NET::WM2DesktopFileName |
        NET::WM2GTKFrameExtents;

    auto wmClientLeaderCookie = fetchWmClientLeader();
    auto skipCloseAnimationCookie = fetchSkipCloseAnimation();
    auto showOnScreenEdgeCookie = fetchShowOnScreenEdge();
    auto colorSchemeCookie = fetchColorScheme();
    auto firstInTabBoxCookie = fetchFirstInTabBox();
    auto transientCookie = fetchTransient();
    auto activitiesCookie = fetchActivities();
    auto applicationMenuServiceNameCookie = fetchApplicationMenuServiceName();
    auto applicationMenuObjectPathCookie = fetchApplicationMenuObjectPath();

    m_geometryHints.init(window());
    m_motif.init(window());
    info = new WinInfo(this, m_client, rootWindow(), properties, properties2);

    if (isDesktop() && bit_depth == 32) {
        // force desktop windows to be opaque. It's a desktop after all, there is no window below
        bit_depth = 24;
    }

    // If it's already mapped, ignore hint
    bool init_minimize = !isMapped && (info->initialMappingState() == NET::Iconic);

    m_colormap = attr->colormap;

    getResourceClass();
    readWmClientLeader(wmClientLeaderCookie);
    getWmClientMachine();
    getSyncCounter();
    // First only read the caption text, so that setupWindowRules() can use it for matching,
    // and only then really set the caption using setCaption(), which checks for duplicates etc.
    // and also relies on rules already existing
    cap_normal = readName();
    setupWindowRules(false);
    setCaption(cap_normal, true);

    connect(this, &X11Client::windowClassChanged, this, &X11Client::evaluateWindowRules);

    if (Xcb::Extensions::self()->isShapeAvailable())
        xcb_shape_select_input(connection(), window(), true);
    detectShape(window());
    detectNoBorder();
    fetchIconicName();
    setClientFrameExtents(info->gtkFrameExtents());

    // Needs to be done before readTransient() because of reading the group
    checkGroup();
    updateUrgency();
    updateAllowedActions(); // Group affects isMinimizable()

    setModal((info->state() & NET::Modal) != 0);   // Needs to be valid before handling groups
    readTransientProperty(transientCookie);
    setDesktopFileName(rules()->checkDesktopFile(QByteArray(info->desktopFileName()), true).toUtf8());
    getIcons();
    connect(this, &X11Client::desktopFileNameChanged, this, &X11Client::getIcons);

    m_geometryHints.read();
    getMotifHints();
    getWmOpaqueRegion();
    readSkipCloseAnimation(skipCloseAnimationCookie);

    // TODO: Try to obey all state information from info->state()

    setOriginalSkipTaskbar((info->state() & NET::SkipTaskbar) != 0);
    setSkipPager((info->state() & NET::SkipPager) != 0);
    setSkipSwitcher((info->state() & NET::SkipSwitcher) != 0);
    readFirstInTabBox(firstInTabBoxCookie);

    setupCompositing();

    KStartupInfoId asn_id;
    KStartupInfoData asn_data;
    bool asn_valid = workspace()->checkStartupNotification(window(), asn_id, asn_data);

    // Make sure that the input window is created before we update the stacking order
    updateInputWindow();

    workspace()->updateClientLayer(this);

    SessionInfo* session = workspace()->takeSessionInfo(this);
    if (session) {
        init_minimize = session->minimized;
        noborder = session->noBorder;
    }

    setShortcut(rules()->checkShortcut(session ? session->shortcut : QString(), true));

    init_minimize = rules()->checkMinimize(init_minimize, !isMapped);
    noborder = rules()->checkNoBorder(noborder, !isMapped);

    readActivities(activitiesCookie);

    // Initial desktop placement
    int desk = 0;
    if (session) {
        desk = session->desktop;
        if (session->onAllDesktops)
            desk = NET::OnAllDesktops;
        setOnActivities(session->activities);
    } else {
        // If this window is transient, ensure that it is opened on the
        // same window as its parent.  this is necessary when an application
        // starts up on a different desktop than is currently displayed
        if (isTransient()) {
            auto mainclients = mainClients();
            bool on_current = false;
            bool on_all = false;
            AbstractClient* maincl = nullptr;
            // This is slightly duplicated from Placement::placeOnMainWindow()
            for (auto it = mainclients.constBegin();
                    it != mainclients.constEnd();
                    ++it) {
                if (mainclients.count() > 1 &&      // A group-transient
                    (*it)->isSpecialWindow() &&     // Don't consider toolbars etc when placing
                    !(info->state() & NET::Modal))  // except when it's modal (blocks specials as well)
                    continue;
                maincl = *it;
                if ((*it)->isOnCurrentDesktop())
                    on_current = true;
                if ((*it)->isOnAllDesktops())
                    on_all = true;
            }
            if (on_all)
                desk = NET::OnAllDesktops;
            else if (on_current)
                desk = VirtualDesktopManager::self()->current();
            else if (maincl != nullptr)
                desk = maincl->desktop();

            if (maincl)
                setOnActivities(maincl->activities());
        } else { // a transient shall appear on its leader and not drag that around
            if (info->desktop())
                desk = info->desktop(); // Window had the initial desktop property, force it
            if (desktop() == 0 && asn_valid && asn_data.desktop() != 0)
                desk = asn_data.desktop();
        }
#ifdef KWIN_BUILD_ACTIVITIES
        if (Activities::self() && !isMapped && !noborder && isNormalWindow() && !activitiesDefined) {
            //a new, regular window, when we're not recovering from a crash,
            //and it hasn't got an activity. let's try giving it the current one.
            //TODO: decide whether to keep this before the 4.6 release
            //TODO: if we are keeping it (at least as an option), replace noborder checking
            //with a public API for setting windows to be on all activities.
            //something like KWindowSystem::setOnAllActivities or
            //KActivityConsumer::setOnAllActivities
            setOnActivity(Activities::self()->current(), true);
        }
#endif
    }

    if (desk == 0)   // Assume window wants to be visible on the current desktop
        desk = isDesktop() ? static_cast<int>(NET::OnAllDesktops) : VirtualDesktopManager::self()->current();
    desk = rules()->checkDesktop(desk, !isMapped);
    if (desk != NET::OnAllDesktops)   // Do range check
        desk = qBound(1, desk, static_cast<int>(VirtualDesktopManager::self()->count()));
    setDesktop(desk);
    info->setDesktop(desk);
    workspace()->updateOnAllDesktopsOfTransients(this);   // SELI TODO
    //onAllDesktopsChange(); // Decoration doesn't exist here yet

    QString activitiesList;
    activitiesList = rules()->checkActivity(activitiesList, !isMapped);
    if (!activitiesList.isEmpty())
        setOnActivities(activitiesList.split(QStringLiteral(",")));

    QRect geom(windowGeometry.rect());
    bool placementDone = false;

    if (session)
        geom = session->geometry;

    QRect area;
    bool partial_keep_in_area = isMapped || session;
    if (isMapped || session) {
        area = workspace()->clientArea(FullArea, geom.center(), desktop());
        checkOffscreenPosition(&geom, area);
    } else {
        int screen = asn_data.xinerama() == -1 ? screens()->current() : asn_data.xinerama();
        screen = rules()->checkScreen(screen, !isMapped);
        area = workspace()->clientArea(PlacementArea, screens()->geometry(screen).center(), desktop());
    }

    if (isDesktop())
        // KWin doesn't manage desktop windows
        placementDone = true;

    bool usePosition = false;
    if (isMapped || session || placementDone)
        placementDone = true; // Use geometry
    else if (isTransient() && !isUtility() && !isDialog() && !isSplash())
        usePosition = true;
    else if (isTransient() && !hasNETSupport())
        usePosition = true;
    else if (isDialog() && hasNETSupport()) {
        // If the dialog is actually non-NETWM transient window, don't try to apply placement to it,
        // it breaks with too many things (xmms, display)
        if (mainClients().count() >= 1) {
#if 1
            // #78082 - Ok, it seems there are after all some cases when an application has a good
            // reason to specify a position for its dialog. Too bad other WMs have never bothered
            // with placement for dialogs, so apps always specify positions for their dialogs,
            // including such silly positions like always centered on the screen or under mouse.
            // Using ignoring requested position in window-specific settings helps, and now
            // there's also _NET_WM_FULL_PLACEMENT.
            usePosition = true;
#else
            ; // Force using placement policy
#endif
        } else
            usePosition = true;
    } else if (isSplash())
        ; // Force using placement policy
    else
        usePosition = true;
    if (!rules()->checkIgnoreGeometry(!usePosition, true)) {
        if (m_geometryHints.hasPosition()) {
            placementDone = true;
            // Disobey xinerama placement option for now (#70943)
            area = workspace()->clientArea(PlacementArea, geom.center(), desktop());
        }
    }

    if (isMovable() && (geom.x() > area.right() || geom.y() > area.bottom()))
        placementDone = false; // Weird, do not trust.

    if (placementDone) {
        QPoint position = geom.topLeft();
        // Session contains the position of the frame geometry before gravitating.
        if (!session) {
            position = clientPosToFramePos(position);
        }
        move(position);
    }

    // Create client group if the window will have a decoration
    bool dontKeepInArea = false;
    readColorScheme(colorSchemeCookie);

    readApplicationMenuServiceName(applicationMenuServiceNameCookie);
    readApplicationMenuObjectPath(applicationMenuObjectPathCookie);

    updateDecoration(false);   // Also gravitates
    // TODO: Is CentralGravity right here, when resizing is done after gravitating?
    plainResize(rules()->checkSize(sizeForClientSize(geom.size()), !isMapped));

    QPoint forced_pos = rules()->checkPosition(invalidPoint, !isMapped);
    if (forced_pos != invalidPoint) {
        move(forced_pos);
        placementDone = true;
        // Don't keep inside workarea if the window has specially configured position
        partial_keep_in_area = true;
        area = workspace()->clientArea(FullArea, geom.center(), desktop());
    }
    if (!placementDone) {
        // Placement needs to be after setting size
        Placement::self()->place(this, area);
        // The client may have been moved to another screen, update placement area.
        area = workspace()->clientArea(PlacementArea, this);
        dontKeepInArea = true;
        placementDone = true;
    }

    // bugs #285967, #286146, #183694
    // geometry() now includes the requested size and the decoration and is at the correct screen/position (hopefully)
    // Maximization for oversized windows must happen NOW.
    // If we effectively pass keepInArea(), the window will resizeWithChecks() - i.e. constrained
    // to the combo of all screen MINUS all struts on the edges
    // If only one screen struts, this will affect screens as a side-effect, the window is artificailly shrinked
    // below the screen size and as result no more maximized what breaks KMainWindow's stupid width+1, height+1 hack
    // TODO: get KMainWindow a correct state storage what will allow to store the restore size as well.

    if (!session) { // has a better handling of this
        geom_restore = frameGeometry(); // Remember restore geometry
        if (isMaximizable() && (width() >= area.width() || height() >= area.height())) {
            // Window is too large for the screen, maximize in the
            // directions necessary
            const QSize ss = workspace()->clientArea(ScreenArea, area.center(), desktop()).size();
            const QRect fsa = workspace()->clientArea(FullArea, geom.center(), desktop());
            const QSize cs = clientSize();
            int pseudo_max = ((info->state() & NET::MaxVert) ? MaximizeVertical : 0) |
                             ((info->state() & NET::MaxHoriz) ? MaximizeHorizontal : 0);
            if (width() >= area.width())
                pseudo_max |=  MaximizeHorizontal;
            if (height() >= area.height())
                pseudo_max |=  MaximizeVertical;

            // heuristics:
            // if decorated client is smaller than the entire screen, the user might want to move it around (multiscreen)
            // in this case, if the decorated client is bigger than the screen (+1), we don't take this as an
            // attempt for maximization, but just constrain the size (the window simply wants to be bigger)
            // NOTICE
            // i intended a second check on cs < area.size() ("the managed client ("minus border") is smaller
            // than the workspace") but gtk / gimp seems to store it's size including the decoration,
            // thus a former maximized window wil become non-maximized
            bool keepInFsArea = false;
            if (width() < fsa.width() && (cs.width() > ss.width()+1)) {
                pseudo_max &= ~MaximizeHorizontal;
                keepInFsArea = true;
            }
            if (height() < fsa.height() && (cs.height() > ss.height()+1)) {
                pseudo_max &= ~MaximizeVertical;
                keepInFsArea = true;
            }

            if (pseudo_max != MaximizeRestore) {
                maximize((MaximizeMode)pseudo_max);
                // from now on, care about maxmode, since the maximization call will override mode for fix aspects
                dontKeepInArea |= (max_mode == MaximizeFull);
                geom_restore = QRect(); // Use placement when unmaximizing ...
                if (!(max_mode & MaximizeVertical)) {
                    geom_restore.setY(y());   // ...but only for horizontal direction
                    geom_restore.setHeight(height());
                }
                if (!(max_mode & MaximizeHorizontal)) {
                    geom_restore.setX(x());   // ...but only for vertical direction
                    geom_restore.setWidth(width());
                }
            }
            if (keepInFsArea)
                keepInArea(fsa, partial_keep_in_area);
        }
    }

    if ((!isSpecialWindow() || isToolbar()) && isMovable() && !dontKeepInArea)
        keepInArea(area, partial_keep_in_area);

    updateShape();

    // CT: Extra check for stupid jdk 1.3.1. But should make sense in general
    // if client has initial state set to Iconic and is transient with a parent
    // window that is not Iconic, set init_state to Normal
    if (init_minimize && isTransient()) {
        auto mainclients = mainClients();
        for (auto it = mainclients.constBegin();
                it != mainclients.constEnd();
                ++it)
            if ((*it)->isShown(true))
                init_minimize = false; // SELI TODO: Even e.g. for NET::Utility?
    }
    // If a dialog is shown for minimized window, minimize it too
    if (!init_minimize && isTransient() && mainClients().count() > 0 &&
            workspace()->sessionManager()->state() != SessionState::Saving) {
        bool visible_parent = false;
        // Use allMainClients(), to include also main clients of group transients
        // that have been optimized out in X11Client::checkGroupTransients()
        auto mainclients = allMainClients();
        for (auto it = mainclients.constBegin();
                it != mainclients.constEnd();
                ++it)
            if ((*it)->isShown(true))
                visible_parent = true;
        if (!visible_parent) {
            init_minimize = true;
            demandAttention();
        }
    }

    if (init_minimize)
        minimize(true);   // No animation

    // Other settings from the previous session
    if (session) {
        // Session restored windows are not considered to be new windows WRT rules,
        // I.e. obey only forcing rules
        setKeepAbove(session->keepAbove);
        setKeepBelow(session->keepBelow);
        setOriginalSkipTaskbar(session->skipTaskbar);
        setSkipPager(session->skipPager);
        setSkipSwitcher(session->skipSwitcher);
        setShade(session->shaded ? ShadeNormal : ShadeNone);
        setOpacity(session->opacity);
        geom_restore = session->restore;
        if (session->maximized != MaximizeRestore) {
            maximize(MaximizeMode(session->maximized));
        }
        if (session->fullscreen != FullScreenNone) {
            setFullScreen(true, false);
            geom_fs_restore = session->fsrestore;
        }
        checkOffscreenPosition(&geom_restore, area);
        checkOffscreenPosition(&geom_fs_restore, area);
    } else {
        // Window may want to be maximized
        // done after checking that the window isn't larger than the workarea, so that
        // the restore geometry from the checks above takes precedence, and window
        // isn't restored larger than the workarea
        MaximizeMode maxmode = static_cast<MaximizeMode>(
                                   ((info->state() & NET::MaxVert) ? MaximizeVertical : 0) |
                                   ((info->state() & NET::MaxHoriz) ? MaximizeHorizontal : 0));
        MaximizeMode forced_maxmode = rules()->checkMaximize(maxmode, !isMapped);

        // Either hints were set to maximize, or is forced to maximize,
        // or is forced to non-maximize and hints were set to maximize
        if (forced_maxmode != MaximizeRestore || maxmode != MaximizeRestore)
            maximize(forced_maxmode);

        // Read other initial states
        setShade(rules()->checkShade(info->state() & NET::Shaded ? ShadeNormal : ShadeNone, !isMapped));
        setKeepAbove(rules()->checkKeepAbove(info->state() & NET::KeepAbove, !isMapped));
        setKeepBelow(rules()->checkKeepBelow(info->state() & NET::KeepBelow, !isMapped));
        setOriginalSkipTaskbar(rules()->checkSkipTaskbar(info->state() & NET::SkipTaskbar, !isMapped));
        setSkipPager(rules()->checkSkipPager(info->state() & NET::SkipPager, !isMapped));
        setSkipSwitcher(rules()->checkSkipSwitcher(info->state() & NET::SkipSwitcher, !isMapped));
        if (info->state() & NET::DemandsAttention)
            demandAttention();
        if (info->state() & NET::Modal)
            setModal(true);

        setFullScreen(rules()->checkFullScreen(info->state() & NET::FullScreen, !isMapped), false);
    }

    updateAllowedActions(true);

    // Set initial user time directly
    m_userTime = readUserTimeMapTimestamp(asn_valid ? &asn_id : nullptr, asn_valid ? &asn_data : nullptr, session);
    group()->updateUserTime(m_userTime);   // And do what X11Client::updateUserTime() does

    // This should avoid flicker, because real restacking is done
    // only after manage() finishes because of blocking, but the window is shown sooner
    m_frame.lower();
    if (session && session->stackingOrder != -1) {
        sm_stacking_order = session->stackingOrder;
        workspace()->restoreSessionStackingOrder(this);
    }

    if (compositing())
        // Sending ConfigureNotify is done when setting mapping state below,
        // Getting the first sync response means window is ready for compositing
        sendSyncRequest();
    else
        ready_for_painting = true; // set to true in case compositing is turned on later. bug #160393

    if (isShown(true)) {
        bool allow;
        if (session)
            allow = session->active &&
                    (!workspace()->wasUserInteraction() || workspace()->activeClient() == nullptr ||
                     workspace()->activeClient()->isDesktop());
        else
            allow = workspace()->allowClientActivation(this, userTime(), false);

        const bool isSessionSaving = workspace()->sessionManager()->state() == SessionState::Saving;

        // If session saving, force showing new windows (i.e. "save file?" dialogs etc.)
        // also force if activation is allowed
        if( !isOnCurrentDesktop() && !isMapped && !session && ( allow || isSessionSaving ))
            VirtualDesktopManager::self()->setCurrent( desktop());

        // If the window is on an inactive activity during session saving, temporarily force it to show.
        if( !isMapped && !session && isSessionSaving && !isOnCurrentActivity()) {
            setSessionActivityOverride( true );
            foreach( AbstractClient* c, mainClients()) {
                if (X11Client *mc = dynamic_cast<X11Client *>(c)) {
                    mc->setSessionActivityOverride(true);
                }
            }
        }

        if (isOnCurrentDesktop() && !isMapped && !allow && (!session || session->stackingOrder < 0))
            workspace()->restackClientUnderActive(this);

        updateVisibility();

        if (!isMapped) {
            if (allow && isOnCurrentDesktop()) {
                if (!isSpecialWindow())
                    if (options->focusPolicyIsReasonable() && wantsTabFocus())
                        workspace()->requestFocus(this);
            } else if (!session && !isSpecialWindow())
                demandAttention();
        }
    } else
        updateVisibility();
    Q_ASSERT(mapping_state != Withdrawn);
    m_managed = true;
    blockGeometryUpdates(false);

    if (m_userTime == XCB_TIME_CURRENT_TIME || m_userTime == -1U) {
        // No known user time, set something old
        m_userTime = xTime() - 1000000;
        if (m_userTime == XCB_TIME_CURRENT_TIME || m_userTime == -1U)   // Let's be paranoid
            m_userTime = xTime() - 1000000 + 10;
    }

    //sendSyntheticConfigureNotify(); // Done when setting mapping state

    delete session;

    discardTemporaryRules();
    applyWindowRules(); // Just in case
    RuleBook::self()->discardUsed(this, false);   // Remove ApplyNow rules
    updateWindowRules(Rules::All); // Was blocked while !isManaged()

    setBlockingCompositing(info->isBlockingCompositing());
    readShowOnScreenEdge(showOnScreenEdgeCookie);

    // Forward all opacity values to the frame in case there'll be other CM running.
    connect(Compositor::self(), &Compositor::compositingToggled, this,
        [this](bool active) {
            if (active) {
                return;
            }
            if (opacity() == 1.0) {
                return;
            }
            NETWinInfo info(connection(), frameId(), rootWindow(), NET::Properties(), NET::Properties2());
            info.setOpacity(static_cast<unsigned long>(opacity() * 0xffffffff));
        }
    );

    // TODO: there's a small problem here - isManaged() depends on the mapping state,
    // but this client is not yet in Workspace's client list at this point, will
    // be only done in addClient()
    emit clientManaging(this);
    return true;
}

// Called only from manage()
void X11Client::embedClient(xcb_window_t w, xcb_visualid_t visualid, xcb_colormap_t colormap, uint8_t depth)
{
    Q_ASSERT(m_client == XCB_WINDOW_NONE);
    Q_ASSERT(frameId() == XCB_WINDOW_NONE);
    Q_ASSERT(m_wrapper == XCB_WINDOW_NONE);
    m_client.reset(w, false);

    const uint32_t zero_value = 0;

    xcb_connection_t *conn = connection();

    // We don't want the window to be destroyed when we quit
    xcb_change_save_set(conn, XCB_SET_MODE_INSERT, m_client);

    m_client.selectInput(zero_value);
    m_client.unmap();
    m_client.setBorderWidth(zero_value);

    // Note: These values must match the order in the xcb_cw_t enum
    const uint32_t cw_values[] = {
        0,                                // back_pixmap
        0,                                // border_pixel
        colormap,                    // colormap
        Cursor::x11Cursor(Qt::ArrowCursor)
    };

    const uint32_t cw_mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL |
                             XCB_CW_COLORMAP | XCB_CW_CURSOR;

    const uint32_t common_event_mask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                                       XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                                       XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                       XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_POINTER_MOTION |
                                       XCB_EVENT_MASK_KEYMAP_STATE |
                                       XCB_EVENT_MASK_FOCUS_CHANGE |
                                       XCB_EVENT_MASK_EXPOSURE |
                                       XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

    const uint32_t frame_event_mask   = common_event_mask | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_VISIBILITY_CHANGE;
    const uint32_t wrapper_event_mask = common_event_mask | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

    const uint32_t client_event_mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE |
                                       XCB_EVENT_MASK_COLOR_MAP_CHANGE |
                                       XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                                       XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;

    // Create the frame window
    xcb_window_t frame = xcb_generate_id(conn);
    xcb_create_window(conn, depth, frame, rootWindow(), 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visualid, cw_mask, cw_values);
    m_frame.reset(frame);

    setWindowHandles(m_client);

    // Create the wrapper window
    xcb_window_t wrapperId = xcb_generate_id(conn);
    xcb_create_window(conn, depth, wrapperId, frame, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visualid, cw_mask, cw_values);
    m_wrapper.reset(wrapperId);

    m_client.reparent(m_wrapper);

    // We could specify the event masks when we create the windows, but the original
    // Xlib code didn't.  Let's preserve that behavior here for now so we don't end up
    // receiving any unexpected events from the wrapper creation or the reparenting.
    m_frame.selectInput(frame_event_mask);
    m_wrapper.selectInput(wrapper_event_mask);
    m_client.selectInput(client_event_mask);

    updateMouseGrab();
}

void X11Client::updateInputWindow()
{
    if (!Xcb::Extensions::self()->isShapeInputAvailable())
        return;

    QRegion region;

    if (!noBorder() && isDecorated()) {
        const QMargins &r = decoration()->resizeOnlyBorders();
        const int left   = r.left();
        const int top    = r.top();
        const int right  = r.right();
        const int bottom = r.bottom();
        if (left != 0 || top != 0 || right != 0 || bottom != 0) {
            region = QRegion(-left,
                             -top,
                             decoration()->size().width() + left + right,
                             decoration()->size().height() + top + bottom);
            region = region.subtracted(decoration()->rect());
        }
    }

    if (region.isEmpty()) {
        m_decoInputExtent.reset();
        return;
    }

    QRect bounds = region.boundingRect();
    input_offset = bounds.topLeft();

    // Move the bounding rect to screen coordinates
    bounds.translate(frameGeometry().topLeft());

    // Move the region to input window coordinates
    region.translate(-input_offset);

    if (!m_decoInputExtent.isValid()) {
        const uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        const uint32_t values[] = {true,
            XCB_EVENT_MASK_ENTER_WINDOW   |
            XCB_EVENT_MASK_LEAVE_WINDOW   |
            XCB_EVENT_MASK_BUTTON_PRESS   |
            XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION
        };
        m_decoInputExtent.create(bounds, XCB_WINDOW_CLASS_INPUT_ONLY, mask, values);
        if (mapping_state == Mapped)
            m_decoInputExtent.map();
    } else {
        m_decoInputExtent.setGeometry(bounds);
    }

    const QVector<xcb_rectangle_t> rects = Xcb::regionToRects(region);
    xcb_shape_rectangles(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         m_decoInputExtent, 0, 0, rects.count(), rects.constData());
}

void X11Client::updateDecoration(bool check_workspace_pos, bool force)
{
    if (!force &&
            ((!isDecorated() && noBorder()) || (isDecorated() && !noBorder())))
        return;
    QRect oldgeom = frameGeometry();
    QRect oldClientGeom = oldgeom.adjusted(borderLeft(), borderTop(), -borderRight(), -borderBottom());
    blockGeometryUpdates(true);
    if (force)
        destroyDecoration();
    if (!noBorder()) {
        createDecoration(oldgeom);
    } else
        destroyDecoration();
    updateShadow();
    if (check_workspace_pos)
        checkWorkspacePosition(oldgeom, -2, oldClientGeom);
    updateInputWindow();
    blockGeometryUpdates(false);
    updateFrameExtents();
}

void X11Client::createDecoration(const QRect& oldgeom)
{
    KDecoration2::Decoration *decoration = Decoration::DecorationBridge::self()->createDecoration(this);
    if (decoration) {
        QMetaObject::invokeMethod(decoration, "update", Qt::QueuedConnection);
        connect(decoration, &KDecoration2::Decoration::shadowChanged, this, &Toplevel::updateShadow);
        connect(decoration, &KDecoration2::Decoration::resizeOnlyBordersChanged, this, &X11Client::updateInputWindow);
        connect(decoration, &KDecoration2::Decoration::bordersChanged, this,
            [this]() {
                updateFrameExtents();
                GeometryUpdatesBlocker blocker(this);
                // TODO: this is obviously idempotent
                // calculateGravitation(true) would have to operate on the old border sizes
//                 move(calculateGravitation(true));
//                 move(calculateGravitation(false));
                QRect oldgeom = frameGeometry();
                plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
                if (!isShade())
                    checkWorkspacePosition(oldgeom);
                emit geometryShapeChanged(this, oldgeom);
            }
        );
        connect(decoratedClient()->decoratedClient(), &KDecoration2::DecoratedClient::widthChanged, this, &X11Client::updateInputWindow);
        connect(decoratedClient()->decoratedClient(), &KDecoration2::DecoratedClient::heightChanged, this, &X11Client::updateInputWindow);
    }
    setDecoration(decoration);

    move(calculateGravitation(false));
    plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
    if (Compositor::compositing()) {
        discardWindowPixmap();
    }
    emit geometryShapeChanged(this, oldgeom);
}

void X11Client::destroyDecoration()
{
    QRect oldgeom = frameGeometry();
    if (isDecorated()) {
        QPoint grav = calculateGravitation(true);
        AbstractClient::destroyDecoration();
        plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
        move(grav);
        if (compositing())
            discardWindowPixmap();
        if (!deleting) {
            emit geometryShapeChanged(this, oldgeom);
        }
    }
    m_decoInputExtent.reset();
}

void X11Client::layoutDecorationRects(QRect &left, QRect &top, QRect &right, QRect &bottom) const
{
    if (!isDecorated()) {
        return;
    }
    QRect r = decoration()->rect();

    NETStrut strut = info->frameOverlap();

    // Ignore the overlap strut when compositing is disabled
    if (!compositing())
        strut.left = strut.top = strut.right = strut.bottom = 0;
    else if (strut.left == -1 && strut.top == -1 && strut.right == -1 && strut.bottom == -1) {
        top = QRect(r.x(), r.y(), r.width(), r.height() / 3);
        left = QRect(r.x(), r.y() + top.height(), width() / 2, r.height() / 3);
        right = QRect(r.x() + left.width(), r.y() + top.height(), r.width() - left.width(), left.height());
        bottom = QRect(r.x(), r.y() + top.height() + left.height(), r.width(), r.height() - left.height() - top.height());
        return;
    }

    top = QRect(r.x(), r.y(), r.width(), borderTop() + strut.top);
    bottom = QRect(r.x(), r.y() + r.height() - borderBottom() - strut.bottom,
                   r.width(), borderBottom() + strut.bottom);
    left = QRect(r.x(), r.y() + top.height(),
                 borderLeft() + strut.left, r.height() - top.height() - bottom.height());
    right = QRect(r.x() + r.width() - borderRight() - strut.right, r.y() + top.height(),
                  borderRight() + strut.right, r.height() - top.height() - bottom.height());
}

QRect X11Client::transparentRect() const
{
    if (isShade())
        return QRect();

    NETStrut strut = info->frameOverlap();
    // Ignore the strut when compositing is disabled or the decoration doesn't support it
    if (!compositing())
        strut.left = strut.top = strut.right = strut.bottom = 0;
    else if (strut.left == -1 && strut.top == -1 && strut.right == -1 && strut.bottom == -1)
        return QRect();

    const QRect r = QRect(clientPos(), clientSize())
                    .adjusted(strut.left, strut.top, -strut.right, -strut.bottom);
    if (r.isValid())
        return r;

    return QRect();
}

void X11Client::detectNoBorder()
{
    if (shape()) {
        noborder = true;
        app_noborder = true;
        return;
    }
    switch(windowType()) {
    case NET::Desktop :
    case NET::Dock :
    case NET::TopMenu :
    case NET::Splash :
    case NET::Notification :
    case NET::OnScreenDisplay :
    case NET::CriticalNotification :
        noborder = true;
        app_noborder = true;
        break;
    case NET::Unknown :
    case NET::Normal :
    case NET::Toolbar :
    case NET::Menu :
    case NET::Dialog :
    case NET::Utility :
        noborder = false;
        break;
    default:
        abort();
    }
    // NET::Override is some strange beast without clear definition, usually
    // just meaning "noborder", so let's treat it only as such flag, and ignore it as
    // a window type otherwise (SUPPORTED_WINDOW_TYPES_MASK doesn't include it)
    if (info->windowType(NET::OverrideMask) == NET::Override) {
        noborder = true;
        app_noborder = true;
    }
}

void X11Client::updateFrameExtents()
{
    NETStrut strut;
    strut.left = borderLeft();
    strut.right = borderRight();
    strut.top = borderTop();
    strut.bottom = borderBottom();
    info->setFrameExtents(strut);
}

void X11Client::setClientFrameExtents(const NETStrut &strut)
{
    const QMargins clientFrameExtents(strut.left, strut.top, strut.right, strut.bottom);
    if (m_clientFrameExtents == clientFrameExtents) {
        return;
    }

    const bool wasClientSideDecorated = isClientSideDecorated();
    m_clientFrameExtents = clientFrameExtents;

    // We should resize the client when its custom frame extents are changed so
    // the logical bounds remain the same. This however means that we will send
    // several configure requests to the application upon restoring it from the
    // maximized or fullscreen state. Notice that a client-side decorated client
    // cannot be shaded, therefore it's okay not to use the adjusted size here.
    setFrameGeometry(frameGeometry());

    if (wasClientSideDecorated != isClientSideDecorated()) {
        emit clientSideDecoratedChanged();
    }

    // This will invalidate the window quads cache.
    emit geometryShapeChanged(this, frameGeometry());
}

/**
 * Resizes the decoration, and makes sure the decoration widget gets resize event
 * even if the size hasn't changed. This is needed to make sure the decoration
 * re-layouts (e.g. when maximization state changes,
 * the decoration may alter some borders, but the actual size
 * of the decoration stays the same).
 */
void X11Client::resizeDecoration()
{
    triggerDecorationRepaint();
    updateInputWindow();
}

bool X11Client::userNoBorder() const
{
    return noborder;
}

bool X11Client::isFullScreenable() const
{
    if (!rules()->checkFullScreen(true)) {
        return false;
    }
    if (rules()->checkStrictGeometry(true)) {
        // check geometry constraints (rule to obey is set)
        const QRect fsarea = workspace()->clientArea(FullScreenArea, this);
        if (sizeForClientSize(fsarea.size(), SizemodeAny, true) != fsarea.size()) {
            return false; // the app wouldn't fit exactly fullscreen geometry due to its strict geometry requirements
        }
    }
    // don't check size constrains - some apps request fullscreen despite requesting fixed size
    return !isSpecialWindow(); // also better disallow only weird types to go fullscreen
}

bool X11Client::noBorder() const
{
    return userNoBorder() || isFullScreen();
}

bool X11Client::userCanSetNoBorder() const
{
    // Client-side decorations and server-side decorations are mutually exclusive.
    if (isClientSideDecorated()) {
        return false;
    }

    return !isFullScreen() && !isShade();
}

void X11Client::setNoBorder(bool set)
{
    if (!userCanSetNoBorder())
        return;
    set = rules()->checkNoBorder(set);
    if (noborder == set)
        return;
    noborder = set;
    updateDecoration(true, false);
    updateWindowRules(Rules::NoBorder);
}

void X11Client::checkNoBorder()
{
    setNoBorder(app_noborder);
}

bool X11Client::wantsShadowToBeRendered() const
{
    return !isFullScreen() && maximizeMode() != MaximizeFull;
}

void X11Client::updateShape()
{
    if (shape()) {
        // Workaround for #19644 - Shaped windows shouldn't have decoration
        if (!app_noborder) {
            // Only when shape is detected for the first time, still let the user to override
            app_noborder = true;
            noborder = rules()->checkNoBorder(true);
            updateDecoration(true);
        }
        if (noBorder()) {
            xcb_shape_combine(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
                              frameId(), clientPos().x(), clientPos().y(), window());
        }
    } else if (app_noborder) {
        xcb_shape_mask(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, frameId(), 0, 0, XCB_PIXMAP_NONE);
        detectNoBorder();
        app_noborder = noborder;
        noborder = rules()->checkNoBorder(noborder || m_motif.noBorder());
        updateDecoration(true);
    }

    // Decoration mask (i.e. 'else' here) setting is done in setMask()
    // when the decoration calls it or when the decoration is created/destroyed
    updateInputShape();
    if (compositing()) {
        addRepaintFull();
        addWorkspaceRepaint(visibleRect());   // In case shape change removes part of this window
    }
    emit geometryShapeChanged(this, frameGeometry());
}

static Xcb::Window shape_helper_window(XCB_WINDOW_NONE);

void X11Client::cleanupX11()
{
    shape_helper_window.reset();
}

void X11Client::updateInputShape()
{
    if (hiddenPreview())   // Sets it to none, don't change
        return;
    if (Xcb::Extensions::self()->isShapeInputAvailable()) {
        // There appears to be no way to find out if a window has input
        // shape set or not, so always propagate the input shape
        // (it's the same like the bounding shape by default).
        // Also, build the shape using a helper window, not directly
        // in the frame window, because the sequence set-shape-to-frame,
        // remove-shape-of-client, add-input-shape-of-client has the problem
        // that after the second step there's a hole in the input shape
        // until the real shape of the client is added and that can make
        // the window lose focus (which is a problem with mouse focus policies)
        // TODO: It seems there is, after all - XShapeGetRectangles() - but maybe this is better
        if (!shape_helper_window.isValid())
            shape_helper_window.create(QRect(0, 0, 1, 1));
        shape_helper_window.resize(m_bufferGeometry.size());
        xcb_connection_t *c = connection();
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, 0, 0, frameId());
        xcb_shape_combine(c, XCB_SHAPE_SO_SUBTRACT, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, clientPos().x(), clientPos().y(), window());
        xcb_shape_combine(c, XCB_SHAPE_SO_UNION, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                          shape_helper_window, clientPos().x(), clientPos().y(), window());
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                          frameId(), 0, 0, shape_helper_window);
    }
}

void X11Client::hideClient(bool hide)
{
    if (hidden == hide)
        return;
    hidden = hide;
    updateVisibility();
}

bool X11Client::setupCompositing()
{
    if (!Toplevel::setupCompositing()){
        return false;
    }
    updateVisibility(); // for internalKeep()
    return true;
}

void X11Client::finishCompositing(ReleaseReason releaseReason)
{
    Toplevel::finishCompositing(releaseReason);
    updateVisibility();
    // for safety in case KWin is just resizing the window
    resetHaveResizeEffect();
}

/**
 * Returns whether the window is minimizable or not
 */
bool X11Client::isMinimizable() const
{
    if (isSpecialWindow() && !isTransient())
        return false;
    if (!rules()->checkMinimize(true))
        return false;

    if (isTransient()) {
        // #66868 - Let other xmms windows be minimized when the mainwindow is minimized
        bool shown_mainwindow = false;
        auto mainclients = mainClients();
        for (auto it = mainclients.constBegin();
                it != mainclients.constEnd();
                ++it)
            if ((*it)->isShown(true))
                shown_mainwindow = true;
        if (!shown_mainwindow)
            return true;
    }
#if 0
    // This is here because kicker's taskbar doesn't provide separate entries
    // for windows with an explicitly given parent
    // TODO: perhaps this should be redone
    // Disabled for now, since at least modal dialogs should be minimizable
    // (resulting in the mainwindow being minimized too).
    if (transientFor() != NULL)
        return false;
#endif
    if (!wantsTabFocus())   // SELI, TODO: - NET::Utility? why wantsTabFocus() - skiptaskbar? ?
        return false;
    return true;
}

void X11Client::doMinimize()
{
    updateVisibility();
    updateAllowedActions();
    workspace()->updateMinimizedOfTransients(this);
}

QRect X11Client::iconGeometry() const
{
    NETRect r = info->iconGeometry();
    QRect geom(r.pos.x, r.pos.y, r.size.width, r.size.height);
    if (geom.isValid())
        return geom;
    else {
        // Check all mainwindows of this window (recursively)
        foreach (AbstractClient * amainwin, mainClients()) {
            X11Client *mainwin = dynamic_cast<X11Client *>(amainwin);
            if (!mainwin) {
                continue;
            }
            geom = mainwin->iconGeometry();
            if (geom.isValid())
                return geom;
        }
        // No mainwindow (or their parents) with icon geometry was found
        return AbstractClient::iconGeometry();
    }
}

bool X11Client::isShadeable() const
{
    return !isSpecialWindow() && !noBorder() && (rules()->checkShade(ShadeNormal) != rules()->checkShade(ShadeNone));
}

void X11Client::setShade(ShadeMode mode)
{
    if (mode == ShadeHover && isMove())
        return; // causes geometry breaks and is probably nasty
    if (isSpecialWindow() || noBorder())
        mode = ShadeNone;
    mode = rules()->checkShade(mode);
    if (shade_mode == mode)
        return;
    bool was_shade = isShade();
    ShadeMode was_shade_mode = shade_mode;
    shade_mode = mode;

    // Decorations may turn off some borders when shaded
    // this has to happen _before_ the tab alignment since it will restrict the minimum geometry
#if 0
    if (decoration)
        decoration->borders(border_left, border_right, border_top, border_bottom);
#endif

    if (was_shade == isShade()) {
        // Decoration may want to update after e.g. hover-shade changes
        emit shadeChanged();
        return; // No real change in shaded state
    }

    Q_ASSERT(isDecorated());   // noborder windows can't be shaded
    GeometryUpdatesBlocker blocker(this);

    // TODO: All this unmapping, resizing etc. feels too much duplicated from elsewhere
    if (isShade()) {
        // shade_mode == ShadeNormal
        addWorkspaceRepaint(visibleRect());
        // Shade
        shade_geometry_change = true;
        QSize s(sizeForClientSize(QSize(clientSize())));
        s.setHeight(borderTop() + borderBottom());
        m_wrapper.selectInput(ClientWinMask);   // Avoid getting UnmapNotify
        m_wrapper.unmap();
        m_client.unmap();
        m_wrapper.selectInput(ClientWinMask | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);
        exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
        plainResize(s);
        shade_geometry_change = false;
        if (was_shade_mode == ShadeHover) {
            if (shade_below && workspace()->stackingOrder().indexOf(shade_below) > -1)
                    workspace()->restack(this, shade_below, true);
            if (isActive())
                workspace()->activateNextClient(this);
        } else if (isActive()) {
            workspace()->focusToNull();
        }
    } else {
        shade_geometry_change = true;
        if (decoratedClient())
            decoratedClient()->signalShadeChange();
        QSize s(sizeForClientSize(clientSize()));
        shade_geometry_change = false;
        plainResize(s);
        setGeometryRestore(frameGeometry());
        if ((shade_mode == ShadeHover || shade_mode == ShadeActivated) && rules()->checkAcceptFocus(info->input()))
            setActive(true);
        if (shade_mode == ShadeHover) {
            QList<Toplevel *> order = workspace()->stackingOrder();
            // invalidate, since "this" could be the topmost toplevel and shade_below dangeling
            shade_below = nullptr;
            // this is likely related to the index parameter?!
            for (int idx = order.indexOf(this) + 1; idx < order.count(); ++idx) {
                shade_below = qobject_cast<X11Client *>(order.at(idx));
                if (shade_below) {
                    break;
                }
            }
            if (shade_below && shade_below->isNormalWindow())
                workspace()->raiseClient(this);
            else
                shade_below = nullptr;
        }
        m_wrapper.map();
        m_client.map();
        exportMappingState(XCB_ICCCM_WM_STATE_NORMAL);
        if (isActive())
            workspace()->requestFocus(this);
    }
    info->setState(isShade() ? NET::Shaded : NET::States(), NET::Shaded);
    info->setState(isShown(false) ? NET::States() : NET::Hidden, NET::Hidden);
    discardWindowPixmap();
    updateVisibility();
    updateAllowedActions();
    updateWindowRules(Rules::Shade);

    emit shadeChanged();
}

void X11Client::shadeHover()
{
    setShade(ShadeHover);
    cancelShadeHoverTimer();
}

void X11Client::shadeUnhover()
{
    setShade(ShadeNormal);
    cancelShadeHoverTimer();
}

void X11Client::cancelShadeHoverTimer()
{
    delete shadeHoverTimer;
    shadeHoverTimer = nullptr;
}

void X11Client::toggleShade()
{
    // If the mode is ShadeHover or ShadeActive, cancel shade too
    setShade(shade_mode == ShadeNone ? ShadeNormal : ShadeNone);
}

void X11Client::updateVisibility()
{
    if (deleting)
        return;
    if (hidden) {
        info->setState(NET::Hidden, NET::Hidden);
        setSkipTaskbar(true);   // Also hide from taskbar
        if (compositing() && options->hiddenPreviews() == HiddenPreviewsAlways)
            internalKeep();
        else
            internalHide();
        return;
    }
    setSkipTaskbar(originalSkipTaskbar());   // Reset from 'hidden'
    if (isMinimized()) {
        info->setState(NET::Hidden, NET::Hidden);
        if (compositing() && options->hiddenPreviews() == HiddenPreviewsAlways)
            internalKeep();
        else
            internalHide();
        return;
    }
    info->setState(NET::States(), NET::Hidden);
    if (!isOnCurrentDesktop()) {
        if (compositing() && options->hiddenPreviews() != HiddenPreviewsNever)
            internalKeep();
        else
            internalHide();
        return;
    }
    if (!isOnCurrentActivity()) {
        if (compositing() && options->hiddenPreviews() != HiddenPreviewsNever)
            internalKeep();
        else
            internalHide();
        return;
    }
    internalShow();
}


/**
 * Sets the client window's mapping state. Possible values are
 * WithdrawnState, IconicState, NormalState.
 */
void X11Client::exportMappingState(int s)
{
    Q_ASSERT(m_client != XCB_WINDOW_NONE);
    Q_ASSERT(!deleting || s == XCB_ICCCM_WM_STATE_WITHDRAWN);
    if (s == XCB_ICCCM_WM_STATE_WITHDRAWN) {
        m_client.deleteProperty(atoms->wm_state);
        return;
    }
    Q_ASSERT(s == XCB_ICCCM_WM_STATE_NORMAL || s == XCB_ICCCM_WM_STATE_ICONIC);

    int32_t data[2];
    data[0] = s;
    data[1] = XCB_NONE;
    m_client.changeProperty(atoms->wm_state, atoms->wm_state, 32, 2, data);
}

void X11Client::internalShow()
{
    if (mapping_state == Mapped)
        return;
    MappingState old = mapping_state;
    mapping_state = Mapped;
    if (old == Unmapped || old == Withdrawn)
        map();
    if (old == Kept) {
        m_decoInputExtent.map();
        updateHiddenPreview();
    }
    emit windowShown(this);
}

void X11Client::internalHide()
{
    if (mapping_state == Unmapped)
        return;
    MappingState old = mapping_state;
    mapping_state = Unmapped;
    if (old == Mapped || old == Kept)
        unmap();
    if (old == Kept)
        updateHiddenPreview();
    addWorkspaceRepaint(visibleRect());
    workspace()->clientHidden(this);
    emit windowHidden(this);
}

void X11Client::internalKeep()
{
    Q_ASSERT(compositing());
    if (mapping_state == Kept)
        return;
    MappingState old = mapping_state;
    mapping_state = Kept;
    if (old == Unmapped || old == Withdrawn)
        map();
    m_decoInputExtent.unmap();
    if (isActive())
        workspace()->focusToNull(); // get rid of input focus, bug #317484
    updateHiddenPreview();
    addWorkspaceRepaint(visibleRect());
    workspace()->clientHidden(this);
}

/**
 * Maps (shows) the client. Note that it is mapping state of the frame,
 * not necessarily the client window itself (i.e. a shaded window is here
 * considered mapped, even though it is in IconicState).
 */
void X11Client::map()
{
    // XComposite invalidates backing pixmaps on unmap (minimize, different
    // virtual desktop, etc.).  We kept the last known good pixmap around
    // for use in effects, but now we want to have access to the new pixmap
    if (compositing())
        discardWindowPixmap();
    m_frame.map();
    if (!isShade()) {
        m_wrapper.map();
        m_client.map();
        m_decoInputExtent.map();
        exportMappingState(XCB_ICCCM_WM_STATE_NORMAL);
    } else
        exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
    addLayerRepaint(visibleRect());
}

/**
 * Unmaps the client. Again, this is about the frame.
 */
void X11Client::unmap()
{
    // Here it may look like a race condition, as some other client might try to unmap
    // the window between these two XSelectInput() calls. However, they're supposed to
    // use XWithdrawWindow(), which also sends a synthetic event to the root window,
    // which won't be missed, so this shouldn't be a problem. The chance the real UnmapNotify
    // will be missed is also very minimal, so I don't think it's needed to grab the server
    // here.
    m_wrapper.selectInput(ClientWinMask);   // Avoid getting UnmapNotify
    m_frame.unmap();
    m_wrapper.unmap();
    m_client.unmap();
    m_decoInputExtent.unmap();
    m_wrapper.selectInput(ClientWinMask | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);
    exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
}

/**
 * XComposite doesn't keep window pixmaps of unmapped windows, which means
 * there wouldn't be any previews of windows that are minimized or on another
 * virtual desktop. Therefore rawHide() actually keeps such windows mapped.
 * However special care needs to be taken so that such windows don't interfere.
 * Therefore they're put very low in the stacking order and they have input shape
 * set to none, which hopefully is enough. If there's no input shape available,
 * then it's hoped that there will be some other desktop above it *shrug*.
 * Using normal shape would be better, but that'd affect other things, e.g. painting
 * of the actual preview.
 */
void X11Client::updateHiddenPreview()
{
    if (hiddenPreview()) {
        workspace()->forceRestacking();
        if (Xcb::Extensions::self()->isShapeInputAvailable()) {
            xcb_shape_rectangles(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                                 XCB_CLIP_ORDERING_UNSORTED, frameId(), 0, 0, 0, nullptr);
        }
    } else {
        workspace()->forceRestacking();
        updateInputShape();
    }
}

void X11Client::sendClientMessage(xcb_window_t w, xcb_atom_t a, xcb_atom_t protocol, uint32_t data1, uint32_t data2, uint32_t data3, xcb_timestamp_t timestamp)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.type = a;
    ev.format = 32;
    ev.data.data32[0] = protocol;
    ev.data.data32[1] = timestamp;
    ev.data.data32[2] = data1;
    ev.data.data32[3] = data2;
    ev.data.data32[4] = data3;
    uint32_t eventMask = 0;
    if (w == rootWindow()) {
        eventMask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT; // Magic!
    }
    xcb_send_event(connection(), false, w, eventMask, reinterpret_cast<const char*>(&ev));
    xcb_flush(connection());
}

/**
 * Returns whether the window may be closed (have a close button)
 */
bool X11Client::isCloseable() const
{
    return rules()->checkCloseable(m_motif.close() && !isSpecialWindow());
}

/**
 * Closes the window by either sending a delete_window message or using XKill.
 */
void X11Client::closeWindow()
{
    if (!isCloseable())
        return;

    // Update user time, because the window may create a confirming dialog.
    updateUserTime();

    if (info->supportsProtocol(NET::DeleteWindowProtocol)) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_delete_window);
        pingWindow();
    } else // Client will not react on wm_delete_window. We have not choice
        // but destroy his connection to the XServer.
        killWindow();
}


/**
 * Kills the window via XKill
 */
void X11Client::killWindow()
{
    qCDebug(KWIN_CORE) << "X11Client::killWindow():" << caption();
    killProcess(false);
    m_client.kill();  // Always kill this client at the server
    destroyClient();
}

/**
 * Send a ping to the window using _NET_WM_PING if possible if it
 * doesn't respond within a reasonable time, it will be killed.
 */
void X11Client::pingWindow()
{
    if (!info->supportsProtocol(NET::PingProtocol))
        return; // Can't ping :(
    if (options->killPingTimeout() == 0)
        return; // Turned off
    if (ping_timer != nullptr)
        return; // Pinging already
    ping_timer = new QTimer(this);
    connect(ping_timer, &QTimer::timeout, this,
        [this]() {
            if (unresponsive()) {
                qCDebug(KWIN_CORE) << "Final ping timeout, asking to kill:" << caption();
                ping_timer->deleteLater();
                ping_timer = nullptr;
                killProcess(true, m_pingTimestamp);
                return;
            }

            qCDebug(KWIN_CORE) << "First ping timeout:" << caption();

            setUnresponsive(true);
            ping_timer->start();
        }
    );
    ping_timer->setSingleShot(true);
    // we'll run the timer twice, at first we'll desaturate the window
    // and the second time we'll show the "do you want to kill" prompt
    ping_timer->start(options->killPingTimeout() / 2);
    m_pingTimestamp = xTime();
    workspace()->sendPingToWindow(window(), m_pingTimestamp);
}

void X11Client::gotPing(xcb_timestamp_t timestamp)
{
    // Just plain compare is not good enough because of 64bit and truncating and whatnot
    if (NET::timestampCompare(timestamp, m_pingTimestamp) != 0)
        return;
    delete ping_timer;
    ping_timer = nullptr;

    setUnresponsive(false);

    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) { // means the process is alive
        ::kill(m_killHelperPID, SIGTERM);
        m_killHelperPID = 0;
    }
}

void X11Client::killProcess(bool ask, xcb_timestamp_t timestamp)
{
    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) // means the process is alive
        return;
    Q_ASSERT(!ask || timestamp != XCB_TIME_CURRENT_TIME);
    pid_t pid = info->pid();
    if (pid <= 0 || clientMachine()->hostName().isEmpty())  // Needed properties missing
        return;
    qCDebug(KWIN_CORE) << "Kill process:" << pid << "(" << clientMachine()->hostName() << ")";
    if (!ask) {
        if (!clientMachine()->isLocal()) {
            QStringList lst;
            lst << QString::fromUtf8(clientMachine()->hostName()) << QStringLiteral("kill") << QString::number(pid);
            QProcess::startDetached(QStringLiteral("xon"), lst);
        } else
            ::kill(pid, SIGTERM);
    } else {
        QString hostname = clientMachine()->isLocal() ? QStringLiteral("localhost") : QString::fromUtf8(clientMachine()->hostName());
        // execute helper from build dir or the system installed one
        const QFileInfo buildDirBinary{QDir{QCoreApplication::applicationDirPath()}, QStringLiteral("kwin_killer_helper")};
        QProcess::startDetached(buildDirBinary.exists() ? buildDirBinary.absoluteFilePath() : QStringLiteral(KWIN_KILLER_BIN),
                                QStringList() << QStringLiteral("--pid") << QString::number(unsigned(pid)) << QStringLiteral("--hostname") << hostname
                                << QStringLiteral("--windowname") << captionNormal()
                                << QStringLiteral("--applicationname") << QString::fromUtf8(resourceClass())
                                << QStringLiteral("--wid") << QString::number(window())
                                << QStringLiteral("--timestamp") << QString::number(timestamp),
                                QString(), &m_killHelperPID);
    }
}

void X11Client::doSetSkipTaskbar()
{
    info->setState(skipTaskbar() ? NET::SkipTaskbar : NET::States(), NET::SkipTaskbar);
}

void X11Client::doSetSkipPager()
{
    info->setState(skipPager() ? NET::SkipPager : NET::States(), NET::SkipPager);
}

void X11Client::doSetSkipSwitcher()
{
    info->setState(skipSwitcher() ? NET::SkipSwitcher : NET::States(), NET::SkipSwitcher);
}

void X11Client::doSetDesktop(int desktop, int was_desk)
{
    Q_UNUSED(desktop)
    Q_UNUSED(was_desk)
    updateVisibility();
}

/**
 * Sets whether the client is on @p activity.
 * If you remove it from its last activity, then it's on all activities.
 *
 * Note: If it was on all activities and you try to remove it from one, nothing will happen;
 * I don't think that's an important enough use case to handle here.
 */
void X11Client::setOnActivity(const QString &activity, bool enable)
{
#ifdef KWIN_BUILD_ACTIVITIES
    if (! Activities::self()) {
        return;
    }
    QStringList newActivitiesList = activities();
    if (newActivitiesList.contains(activity) == enable)   //nothing to do
        return;
    if (enable) {
        QStringList allActivities = Activities::self()->all();
        if (!allActivities.contains(activity))   //bogus ID
            return;
        newActivitiesList.append(activity);
    } else
        newActivitiesList.removeOne(activity);
    setOnActivities(newActivitiesList);
#else
    Q_UNUSED(activity)
    Q_UNUSED(enable)
#endif
}

/**
 * set exactly which activities this client is on
 */
void X11Client::setOnActivities(QStringList newActivitiesList)
{
#ifdef KWIN_BUILD_ACTIVITIES
    if (!Activities::self()) {
        return;
    }
    QString joinedActivitiesList = newActivitiesList.join(QStringLiteral(","));
    joinedActivitiesList = rules()->checkActivity(joinedActivitiesList, false);
    newActivitiesList = joinedActivitiesList.split(u',', QString::SkipEmptyParts);

    QStringList allActivities = Activities::self()->all();

    auto it = newActivitiesList.begin();
    while (it != newActivitiesList.end()) {
        if (! allActivities.contains(*it)) {
            it = newActivitiesList.erase(it);
        } else {
            it++;
        }
    }

    if (// If we got the request to be on all activities explicitly
        newActivitiesList.isEmpty() || joinedActivitiesList == Activities::nullUuid() ||
        // If we got a list of activities that covers all activities
        (newActivitiesList.count() > 1 && newActivitiesList.count() == allActivities.count())) {

        activityList.clear();
        const QByteArray nullUuid = Activities::nullUuid().toUtf8();
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, nullUuid.length(), nullUuid.constData());

    } else {
        QByteArray joined = joinedActivitiesList.toLatin1();
        activityList = newActivitiesList;
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, joined.length(), joined.constData());
    }

    updateActivities(false);
#else
    Q_UNUSED(newActivitiesList)
#endif
}

void X11Client::blockActivityUpdates(bool b)
{
    if (b) {
        ++m_activityUpdatesBlocked;
    } else {
        Q_ASSERT(m_activityUpdatesBlocked);
        --m_activityUpdatesBlocked;
        if (!m_activityUpdatesBlocked)
            updateActivities(m_blockedActivityUpdatesRequireTransients);
    }
}

/**
 * update after activities changed
 */
void X11Client::updateActivities(bool includeTransients)
{
    if (m_activityUpdatesBlocked) {
        m_blockedActivityUpdatesRequireTransients |= includeTransients;
        return;
    }
    emit activitiesChanged(this);
    m_blockedActivityUpdatesRequireTransients = false; // reset
    FocusChain::self()->update(this, FocusChain::MakeFirst);
    updateVisibility();
    updateWindowRules(Rules::Activity);
}

/**
 * Returns the list of activities the client window is on.
 * if it's on all activities, the list will be empty.
 * Don't use this, use isOnActivity() and friends (from class Toplevel)
 */
QStringList X11Client::activities() const
{
    if (sessionActivityOverride) {
        return QStringList();
    }
    return activityList;
}

/**
 * if @p on is true, sets on all activities.
 * if it's false, sets it to only be on the current activity
 */
void X11Client::setOnAllActivities(bool on)
{
#ifdef KWIN_BUILD_ACTIVITIES
    if (on == isOnAllActivities())
        return;
    if (on) {
        setOnActivities(QStringList());

    } else {
        setOnActivity(Activities::self()->current(), true);
    }
#else
    Q_UNUSED(on)
#endif
}

/**
 * Performs the actual focusing of the window using XSetInputFocus and WM_TAKE_FOCUS
 */
void X11Client::takeFocus()
{
    if (rules()->checkAcceptFocus(info->input()))
        m_client.focus();
    else
        demandAttention(false); // window cannot take input, at least withdraw urgency
    if (info->supportsProtocol(NET::TakeFocusProtocol)) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_take_focus, 0, 0, 0, XCB_CURRENT_TIME);
    }
    workspace()->setShouldGetFocus(this);

    bool breakShowingDesktop = !keepAbove();
    if (breakShowingDesktop) {
        foreach (const X11Client *c, group()->members()) {
            if (c->isDesktop()) {
                breakShowingDesktop = false;
                break;
            }
        }
    }
    if (breakShowingDesktop)
        workspace()->setShowingDesktop(false);
}

/**
 * Returns whether the window provides context help or not. If it does,
 * you should show a help menu item or a help button like '?' and call
 * contextHelp() if this is invoked.
 *
 * \sa contextHelp()
 */
bool X11Client::providesContextHelp() const
{
    return info->supportsProtocol(NET::ContextHelpProtocol);
}

/**
 * Invokes context help on the window. Only works if the window
 * actually provides context help.
 *
 * \sa providesContextHelp()
 */
void X11Client::showContextHelp()
{
    if (info->supportsProtocol(NET::ContextHelpProtocol)) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_context_help);
    }
}

/**
 * Fetches the window's caption (WM_NAME property). It will be
 * stored in the client's caption().
 */
void X11Client::fetchName()
{
    setCaption(readName());
}

static inline QString readNameProperty(xcb_window_t w, xcb_atom_t atom)
{
    const auto cookie = xcb_icccm_get_text_property_unchecked(connection(), w, atom);
    xcb_icccm_get_text_property_reply_t reply;
    if (xcb_icccm_get_wm_name_reply(connection(), cookie, &reply, nullptr)) {
        QString retVal;
        if (reply.encoding == atoms->utf8_string) {
            retVal = QString::fromUtf8(QByteArray(reply.name, reply.name_len));
        } else if (reply.encoding == XCB_ATOM_STRING) {
            retVal = QString::fromLocal8Bit(QByteArray(reply.name, reply.name_len));
        }
        xcb_icccm_get_text_property_reply_wipe(&reply);
        return retVal.simplified();
    }
    return QString();
}

QString X11Client::readName() const
{
    if (info->name() && info->name()[0] != '\0')
        return QString::fromUtf8(info->name()).simplified();
    else {
        return readNameProperty(window(), XCB_ATOM_WM_NAME);
    }
}

// The list is taken from https://www.unicode.org/reports/tr9/ (#154840)
static const QChar LRM(0x200E);

void X11Client::setCaption(const QString& _s, bool force)
{
    QString s(_s);
    for (int i = 0; i < s.length(); ) {
        if (!s[i].isPrint()) {
            if (QChar(s[i]).isHighSurrogate() && i + 1 < s.length() && QChar(s[i + 1]).isLowSurrogate()) {
                const uint uc = QChar::surrogateToUcs4(s[i], s[i + 1]);
                if (!QChar::isPrint(uc)) {
                    s.remove(i, 2);
                } else {
                    i += 2;
                }
                continue;
            }
            s.remove(i, 1);
            continue;
        }
        ++i;
    }
    const bool changed = (s != cap_normal);
    if (!force && !changed) {
        return;
    }
    cap_normal = s;
    if (!force && !changed) {
        emit captionChanged();
        return;
    }

    bool reset_name = force;
    bool was_suffix = (!cap_suffix.isEmpty());
    cap_suffix.clear();
    QString machine_suffix;
    if (!options->condensedTitle()) { // machine doesn't qualify for "clean"
        if (clientMachine()->hostName() != ClientMachine::localhost() && !clientMachine()->isLocal())
            machine_suffix = QLatin1String(" <@") + QString::fromUtf8(clientMachine()->hostName()) + QLatin1Char('>') + LRM;
    }
    QString shortcut_suffix = shortcutCaptionSuffix();
    cap_suffix = machine_suffix + shortcut_suffix;
    if ((!isSpecialWindow() || isToolbar()) && findClientWithSameCaption()) {
        int i = 2;
        do {
            cap_suffix = machine_suffix + QLatin1String(" <") + QString::number(i) + QLatin1Char('>') + LRM;
            i++;
        } while (findClientWithSameCaption());
        info->setVisibleName(caption().toUtf8().constData());
        reset_name = false;
    }
    if ((was_suffix && cap_suffix.isEmpty()) || reset_name) {
        // If it was new window, it may have old value still set, if the window is reused
        info->setVisibleName("");
        info->setVisibleIconName("");
    } else if (!cap_suffix.isEmpty() && !cap_iconic.isEmpty())
        // Keep the same suffix in iconic name if it's set
        info->setVisibleIconName(QString(cap_iconic + cap_suffix).toUtf8().constData());

    emit captionChanged();
}

void X11Client::updateCaption()
{
    setCaption(cap_normal, true);
}

void X11Client::fetchIconicName()
{
    QString s;
    if (info->iconName() && info->iconName()[0] != '\0')
        s = QString::fromUtf8(info->iconName());
    else
        s = readNameProperty(window(), XCB_ATOM_WM_ICON_NAME);
    if (s != cap_iconic) {
        bool was_set = !cap_iconic.isEmpty();
        cap_iconic = s;
        if (!cap_suffix.isEmpty()) {
            if (!cap_iconic.isEmpty())  // Keep the same suffix in iconic name if it's set
                info->setVisibleIconName(QString(s + cap_suffix).toUtf8().constData());
            else if (was_set)
                info->setVisibleIconName("");
        }
    }
}

void X11Client::setClientShown(bool shown)
{
    if (deleting)
        return; // Don't change shown status if this client is being deleted
    if (shown != hidden)
        return; // nothing to change
    hidden = !shown;
    if (shown) {
        map();
        takeFocus();
        autoRaise();
        FocusChain::self()->update(this, FocusChain::MakeFirst);
    } else {
        unmap();
        // Don't move tabs to the end of the list when another tab get's activated
        FocusChain::self()->update(this, FocusChain::MakeLast);
        addWorkspaceRepaint(visibleRect());
    }
}

void X11Client::getMotifHints()
{
    const bool wasClosable = m_motif.close();
    const bool wasNoBorder = m_motif.noBorder();
    if (m_managed) // only on property change, initial read is prefetched
        m_motif.fetch();
    m_motif.read();
    if (m_motif.hasDecoration() && m_motif.noBorder() != wasNoBorder) {
        // If we just got a hint telling us to hide decorations, we do so.
        if (m_motif.noBorder())
            noborder = rules()->checkNoBorder(true);
        // If the Motif hint is now telling us to show decorations, we only do so if the app didn't
        // instruct us to hide decorations in some other way, though.
        else if (!app_noborder)
            noborder = rules()->checkNoBorder(false);
    }

    // mminimize; - Ignore, bogus - E.g. shading or sending to another desktop is "minimizing" too
    // mmaximize; - Ignore, bogus - Maximizing is basically just resizing
    const bool closabilityChanged = wasClosable != m_motif.close();
    if (isManaged())
        updateDecoration(true);   // Check if noborder state has changed
    if (closabilityChanged) {
        emit closeableChanged(isCloseable());
    }
}

void X11Client::getIcons()
{
    // First read icons from the window itself
    const QString themedIconName = iconFromDesktopFile();
    if (!themedIconName.isEmpty()) {
        setIcon(QIcon::fromTheme(themedIconName));
        return;
    }
    QIcon icon;
    auto readIcon = [this, &icon](int size, bool scale = true) {
        const QPixmap pix = KWindowSystem::icon(window(), size, size, scale, KWindowSystem::NETWM | KWindowSystem::WMHints, info);
        if (!pix.isNull()) {
            icon.addPixmap(pix);
        }
    };
    readIcon(16);
    readIcon(32);
    readIcon(48, false);
    readIcon(64, false);
    readIcon(128, false);
    if (icon.isNull()) {
        // Then try window group
        icon = group()->icon();
    }
    if (icon.isNull() && isTransient()) {
        // Then mainclients
        auto mainclients = mainClients();
        for (auto it = mainclients.constBegin();
                it != mainclients.constEnd() && icon.isNull();
                ++it) {
            if (!(*it)->icon().isNull()) {
                icon = (*it)->icon();
                break;
            }
        }
    }
    if (icon.isNull()) {
        // And if nothing else, load icon from classhint or xapp icon
        icon.addPixmap(KWindowSystem::icon(window(),  32,  32,  true, KWindowSystem::ClassHint | KWindowSystem::XApp, info));
        icon.addPixmap(KWindowSystem::icon(window(),  16,  16,  true, KWindowSystem::ClassHint | KWindowSystem::XApp, info));
        icon.addPixmap(KWindowSystem::icon(window(),  64,  64, false, KWindowSystem::ClassHint | KWindowSystem::XApp, info));
        icon.addPixmap(KWindowSystem::icon(window(), 128, 128, false, KWindowSystem::ClassHint | KWindowSystem::XApp, info));
    }
    setIcon(icon);
}

void X11Client::getSyncCounter()
{
    // TODO: make sync working on XWayland
    static const bool isX11 = kwinApp()->operationMode() == Application::OperationModeX11;
    if (!Xcb::Extensions::self()->isSyncAvailable() || !isX11)
        return;

    Xcb::Property syncProp(false, window(), atoms->net_wm_sync_request_counter, XCB_ATOM_CARDINAL, 0, 1);
    const xcb_sync_counter_t counter = syncProp.value<xcb_sync_counter_t>(XCB_NONE);
    if (counter != XCB_NONE) {
        syncRequest.counter = counter;
        syncRequest.value.hi = 0;
        syncRequest.value.lo = 0;
        auto *c = connection();
        xcb_sync_set_counter(c, syncRequest.counter, syncRequest.value);
        if (syncRequest.alarm == XCB_NONE) {
            const uint32_t mask = XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE | XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_EVENTS;
            const uint32_t values[] = {
                syncRequest.counter,
                XCB_SYNC_VALUETYPE_RELATIVE,
                XCB_SYNC_TESTTYPE_POSITIVE_TRANSITION,
                1
            };
            syncRequest.alarm = xcb_generate_id(c);
            auto cookie = xcb_sync_create_alarm_checked(c, syncRequest.alarm, mask, values);
            ScopedCPointer<xcb_generic_error_t> error(xcb_request_check(c, cookie));
            if (!error.isNull()) {
                syncRequest.alarm = XCB_NONE;
            } else {
                xcb_sync_change_alarm_value_list_t value;
                memset(&value, 0, sizeof(value));
                value.value.hi = 0;
                value.value.lo = 1;
                value.delta.hi = 0;
                value.delta.lo = 1;
                xcb_sync_change_alarm_aux(c, syncRequest.alarm, XCB_SYNC_CA_DELTA | XCB_SYNC_CA_VALUE, &value);
            }
        }
    }
}

/**
 * Send the client a _NET_SYNC_REQUEST
 */
void X11Client::sendSyncRequest()
{
    if (syncRequest.counter == XCB_NONE || syncRequest.isPending)
        return; // do NOT, NEVER send a sync request when there's one on the stack. the clients will just stop respoding. FOREVER! ...

    if (!syncRequest.failsafeTimeout) {
        syncRequest.failsafeTimeout = new QTimer(this);
        connect(syncRequest.failsafeTimeout, &QTimer::timeout, this,
            [this]() {
                // client does not respond to XSYNC requests in reasonable time, remove support
                if (!ready_for_painting) {
                    // failed on initial pre-show request
                    setReadyForPainting();
                    setupWindowManagementInterface();
                    return;
                }
                // failed during resize
                syncRequest.isPending = false;
                syncRequest.counter = syncRequest.alarm = XCB_NONE;
                delete syncRequest.timeout; delete syncRequest.failsafeTimeout;
                syncRequest.timeout = syncRequest.failsafeTimeout = nullptr;
                syncRequest.lastTimestamp = XCB_CURRENT_TIME;
            }
        );
        syncRequest.failsafeTimeout->setSingleShot(true);
    }
    // if there's no response within 10 seconds, sth. went wrong and we remove XSYNC support from this client.
    // see events.cpp X11Client::syncEvent()
    syncRequest.failsafeTimeout->start(ready_for_painting ? 10000 : 1000);

    // We increment before the notify so that after the notify
    // syncCounterSerial will equal the value we are expecting
    // in the acknowledgement
    const uint32_t oldLo = syncRequest.value.lo;
    syncRequest.value.lo++;;
    if (oldLo > syncRequest.value.lo) {
        syncRequest.value.hi++;
    }
    if (syncRequest.lastTimestamp >= xTime()) {
        updateXTime();
    }

    // Send the message to client
    sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_sync_request, syncRequest.value.lo, syncRequest.value.hi);
    syncRequest.isPending = true;
    syncRequest.lastTimestamp = xTime();
}

bool X11Client::wantsInput() const
{
    return rules()->checkAcceptFocus(acceptsFocus() || info->supportsProtocol(NET::TakeFocusProtocol));
}

bool X11Client::acceptsFocus() const
{
    return info->input();
}

void X11Client::setBlockingCompositing(bool block)
{
    const bool usedToBlock = blocks_compositing;
    blocks_compositing = rules()->checkBlockCompositing(block && options->windowsBlockCompositing());
    if (usedToBlock != blocks_compositing) {
        emit blockingCompositingChanged(blocks_compositing ? this : nullptr);
    }
}

void X11Client::updateAllowedActions(bool force)
{
    if (!isManaged() && !force)
        return;
    NET::Actions old_allowed_actions = NET::Actions(allowed_actions);
    allowed_actions = NET::Actions();
    if (isMovable())
        allowed_actions |= NET::ActionMove;
    if (isResizable())
        allowed_actions |= NET::ActionResize;
    if (isMinimizable())
        allowed_actions |= NET::ActionMinimize;
    if (isShadeable())
        allowed_actions |= NET::ActionShade;
    // Sticky state not supported
    if (isMaximizable())
        allowed_actions |= NET::ActionMax;
    if (userCanSetFullScreen())
        allowed_actions |= NET::ActionFullScreen;
    allowed_actions |= NET::ActionChangeDesktop; // Always (Pagers shouldn't show Docks etc.)
    if (isCloseable())
        allowed_actions |= NET::ActionClose;
    if (old_allowed_actions == allowed_actions)
        return;
    // TODO: This could be delayed and compressed - It's only for pagers etc. anyway
    info->setAllowedActions(allowed_actions);

    // ONLY if relevant features have changed (and the window didn't just get/loose moveresize for maximization state changes)
    const NET::Actions relevant = ~(NET::ActionMove|NET::ActionResize);
    if ((allowed_actions & relevant) != (old_allowed_actions & relevant)) {
        if ((allowed_actions & NET::ActionMinimize) != (old_allowed_actions & NET::ActionMinimize)) {
            emit minimizeableChanged(allowed_actions & NET::ActionMinimize);
        }
        if ((allowed_actions & NET::ActionShade) != (old_allowed_actions & NET::ActionShade)) {
            emit shadeableChanged(allowed_actions & NET::ActionShade);
        }
        if ((allowed_actions & NET::ActionMax) != (old_allowed_actions & NET::ActionMax)) {
            emit maximizeableChanged(allowed_actions & NET::ActionMax);
        }
    }
}

void X11Client::debug(QDebug& stream) const
{
    stream.nospace();
    print<QDebug>(stream);
}

Xcb::StringProperty X11Client::fetchActivities() const
{
#ifdef KWIN_BUILD_ACTIVITIES
    return Xcb::StringProperty(window(), atoms->activities);
#else
    return Xcb::StringProperty();
#endif
}

void X11Client::readActivities(Xcb::StringProperty &property)
{
#ifdef KWIN_BUILD_ACTIVITIES
    QStringList newActivitiesList;
    QString prop = QString::fromUtf8(property);
    activitiesDefined = !prop.isEmpty();

    if (prop == Activities::nullUuid()) {
        //copied from setOnAllActivities to avoid a redundant XChangeProperty.
        if (!activityList.isEmpty()) {
            activityList.clear();
            updateActivities(true);
        }
        return;
    }
    if (prop.isEmpty()) {
        //note: this makes it *act* like it's on all activities but doesn't set the property to 'ALL'
        if (!activityList.isEmpty()) {
            activityList.clear();
            updateActivities(true);
        }
        return;
    }

    newActivitiesList = prop.split(u',');

    if (newActivitiesList == activityList)
        return; //expected change, it's ok.

    //otherwise, somebody else changed it. we need to validate before reacting.
    //if the activities are not synced, and there are existing clients with
    //activities specified, somebody has restarted kwin. we can not validate
    //activities in this case. we need to trust the old values.
    if (Activities::self() && Activities::self()->serviceStatus() != KActivities::Consumer::Unknown) {
        QStringList allActivities = Activities::self()->all();
        if (allActivities.isEmpty()) {
            qCDebug(KWIN_CORE) << "no activities!?!?";
            //don't touch anything, there's probably something bad going on and we don't wanna make it worse
            return;
        }


        for (int i = 0; i < newActivitiesList.size(); ++i) {
            if (! allActivities.contains(newActivitiesList.at(i))) {
                qCDebug(KWIN_CORE) << "invalid:" << newActivitiesList.at(i);
                newActivitiesList.removeAt(i--);
            }
        }
    }
    setOnActivities(newActivitiesList);
#else
    Q_UNUSED(property)
#endif
}

void X11Client::checkActivities()
{
#ifdef KWIN_BUILD_ACTIVITIES
    Xcb::StringProperty property = fetchActivities();
    readActivities(property);
#endif
}

void X11Client::setSessionActivityOverride(bool needed)
{
    sessionActivityOverride = needed;
    updateActivities(false);
}

QRect X11Client::decorationRect() const
{
    return QRect(0, 0, width(), height());
}

Xcb::Property X11Client::fetchFirstInTabBox() const
{
    return Xcb::Property(false, m_client, atoms->kde_first_in_window_list,
                         atoms->kde_first_in_window_list, 0, 1);
}

void X11Client::readFirstInTabBox(Xcb::Property &property)
{
    setFirstInTabBox(property.toBool(32, atoms->kde_first_in_window_list));
}

void X11Client::updateFirstInTabBox()
{
    // TODO: move into KWindowInfo
    Xcb::Property property = fetchFirstInTabBox();
    readFirstInTabBox(property);
}

Xcb::StringProperty X11Client::fetchColorScheme() const
{
    return Xcb::StringProperty(m_client, atoms->kde_color_sheme);
}

void X11Client::readColorScheme(Xcb::StringProperty &property)
{
    AbstractClient::updateColorScheme(rules()->checkDecoColor(QString::fromUtf8(property)));
}

void X11Client::updateColorScheme()
{
    Xcb::StringProperty property = fetchColorScheme();
    readColorScheme(property);
}

bool X11Client::isClient() const
{
    return true;
}

NET::WindowType X11Client::windowType(bool direct, int supportedTypes) const
{
    // TODO: does it make sense to cache the returned window type for SUPPORTED_MANAGED_WINDOW_TYPES_MASK?
    if (supportedTypes == 0) {
        supportedTypes = SUPPORTED_MANAGED_WINDOW_TYPES_MASK;
    }
    NET::WindowType wt = info->windowType(NET::WindowTypes(supportedTypes));
    if (direct) {
        return wt;
    }
    NET::WindowType wt2 = rules()->checkType(wt);
    if (wt != wt2) {
        wt = wt2;
        info->setWindowType(wt);   // force hint change
    }
    // hacks here
    if (wt == NET::Unknown)   // this is more or less suggested in NETWM spec
        wt = isTransient() ? NET::Dialog : NET::Normal;
    return wt;
}

void X11Client::cancelFocusOutTimer()
{
    if (m_focusOutTimer) {
        m_focusOutTimer->stop();
    }
}

xcb_window_t X11Client::frameId() const
{
    return m_frame;
}

QRect X11Client::bufferGeometry() const
{
    return m_bufferGeometry;
}

QMargins X11Client::bufferMargins() const
{
    return QMargins(borderLeft(), borderTop(), borderRight(), borderBottom());
}

QPoint X11Client::framePosToClientPos(const QPoint &point) const
{
    int x = point.x();
    int y = point.y();

    if (isDecorated()) {
        x += borderLeft();
        y += borderTop();
    } else {
        x -= m_clientFrameExtents.left();
        y -= m_clientFrameExtents.top();
    }

    return QPoint(x, y);
}

QPoint X11Client::clientPosToFramePos(const QPoint &point) const
{
    int x = point.x();
    int y = point.y();

    if (isDecorated()) {
        x -= borderLeft();
        y -= borderTop();
    } else {
        x += m_clientFrameExtents.left();
        y += m_clientFrameExtents.top();
    }

    return QPoint(x, y);
}

QSize X11Client::frameSizeToClientSize(const QSize &size) const
{
    int width = size.width();
    int height = size.height();

    if (isDecorated()) {
        width -= borderLeft() + borderRight();
        height -= borderTop() + borderBottom();
    } else {
        width += m_clientFrameExtents.left() + m_clientFrameExtents.right();
        height += m_clientFrameExtents.top() + m_clientFrameExtents.bottom();
    }

    return QSize(width, height);
}

QSize X11Client::clientSizeToFrameSize(const QSize &size) const
{
    int width = size.width();
    int height = size.height();

    if (isDecorated()) {
        width += borderLeft() + borderRight();
        height += borderTop() + borderBottom();
    } else {
        width -= m_clientFrameExtents.left() + m_clientFrameExtents.right();
        height -= m_clientFrameExtents.top() + m_clientFrameExtents.bottom();
    }

    return QSize(width, height);
}

Xcb::Property X11Client::fetchShowOnScreenEdge() const
{
    return Xcb::Property(false, window(), atoms->kde_screen_edge_show, XCB_ATOM_CARDINAL, 0, 1);
}

void X11Client::readShowOnScreenEdge(Xcb::Property &property)
{
    //value comes in two parts, edge in the lower byte
    //then the type in the upper byte
    // 0 = autohide
    // 1 = raise in front on activate

    const uint32_t value = property.value<uint32_t>(ElectricNone);
    ElectricBorder border = ElectricNone;
    switch (value & 0xFF) {
    case 0:
        border = ElectricTop;
        break;
    case 1:
        border = ElectricRight;
        break;
    case 2:
        border = ElectricBottom;
        break;
    case 3:
        border = ElectricLeft;
        break;
    }
    if (border != ElectricNone) {
        disconnect(m_edgeRemoveConnection);
        disconnect(m_edgeGeometryTrackingConnection);
        bool successfullyHidden = false;

        if (((value >> 8) & 0xFF) == 1) {
            setKeepBelow(true);
            successfullyHidden = keepBelow(); //request could have failed due to user kwin rules

            m_edgeRemoveConnection = connect(this, &AbstractClient::keepBelowChanged, this, [this](){
                if (!keepBelow()) {
                    ScreenEdges::self()->reserve(this, ElectricNone);
                }
            });
        } else {
            hideClient(true);
            successfullyHidden = isHiddenInternal();

            m_edgeGeometryTrackingConnection = connect(this, &X11Client::geometryChanged, this, [this, border](){
                hideClient(true);
                ScreenEdges::self()->reserve(this, border);
            });
        }

        if (successfullyHidden) {
            ScreenEdges::self()->reserve(this, border);
        } else {
            ScreenEdges::self()->reserve(this, ElectricNone);
        }
    } else if (!property.isNull() && property->type != XCB_ATOM_NONE) {
        // property value is incorrect, delete the property
        // so that the client knows that it is not hidden
        xcb_delete_property(connection(), window(), atoms->kde_screen_edge_show);
    } else {
        // restore
        // TODO: add proper unreserve

        //this will call showOnScreenEdge to reset the state
        disconnect(m_edgeGeometryTrackingConnection);
        ScreenEdges::self()->reserve(this, ElectricNone);
    }
}

void X11Client::updateShowOnScreenEdge()
{
    Xcb::Property property = fetchShowOnScreenEdge();
    readShowOnScreenEdge(property);
}

void X11Client::showOnScreenEdge()
{
    disconnect(m_edgeRemoveConnection);

    hideClient(false);
    setKeepBelow(false);
    xcb_delete_property(connection(), window(), atoms->kde_screen_edge_show);
}

void X11Client::addDamage(const QRegion &damage)
{
    if (!ready_for_painting) { // avoid "setReadyForPainting()" function calling overhead
        if (syncRequest.counter == XCB_NONE) {  // cannot detect complete redraw, consider done now
            setReadyForPainting();
            setupWindowManagementInterface();
        }
    }
    repaints_region += damage.translated(bufferGeometry().topLeft() - frameGeometry().topLeft());
    Toplevel::addDamage(damage);
}

bool X11Client::belongsToSameApplication(const AbstractClient *other, SameApplicationChecks checks) const
{
    const X11Client *c2 = dynamic_cast<const X11Client *>(other);
    if (!c2) {
        return false;
    }
    return X11Client::belongToSameApplication(this, c2, checks);
}

QSize X11Client::resizeIncrements() const
{
    return m_geometryHints.resizeIncrements();
}

Xcb::StringProperty X11Client::fetchApplicationMenuServiceName() const
{
    return Xcb::StringProperty(m_client, atoms->kde_net_wm_appmenu_service_name);
}

void X11Client::readApplicationMenuServiceName(Xcb::StringProperty &property)
{
    updateApplicationMenuServiceName(QString::fromUtf8(property));
}

void X11Client::checkApplicationMenuServiceName()
{
    Xcb::StringProperty property = fetchApplicationMenuServiceName();
    readApplicationMenuServiceName(property);
}

Xcb::StringProperty X11Client::fetchApplicationMenuObjectPath() const
{
    return Xcb::StringProperty(m_client, atoms->kde_net_wm_appmenu_object_path);
}

void X11Client::readApplicationMenuObjectPath(Xcb::StringProperty &property)
{
    updateApplicationMenuObjectPath(QString::fromUtf8(property));
}

void X11Client::checkApplicationMenuObjectPath()
{
    Xcb::StringProperty property = fetchApplicationMenuObjectPath();
    readApplicationMenuObjectPath(property);
}

void X11Client::handleSync()
{
    setReadyForPainting();
    setupWindowManagementInterface();
    syncRequest.isPending = false;
    if (syncRequest.failsafeTimeout)
        syncRequest.failsafeTimeout->stop();
    if (isResize()) {
        if (syncRequest.timeout)
            syncRequest.timeout->stop();
        performMoveResize();
    } else // setReadyForPainting does as well, but there's a small chance for resize syncs after the resize ended
        addRepaintFull();
}

void X11Client::move(int x, int y, ForceGeometry_t force)
{
    const QPoint framePosition(x, y);
    m_clientGeometry.moveTopLeft(framePosToClientPos(framePosition));
    const QPoint bufferPosition = isDecorated() ? framePosition : m_clientGeometry.topLeft();
    // resuming geometry updates is handled only in setGeometry()
    Q_ASSERT(pendingGeometryUpdate() == PendingGeometryNone || areGeometryUpdatesBlocked());
    if (!areGeometryUpdatesBlocked() && framePosition != rules()->checkPosition(framePosition)) {
        qCDebug(KWIN_CORE) << "forced position fail:" << framePosition << ":" << rules()->checkPosition(framePosition);
    }
    m_frameGeometry.moveTopLeft(framePosition);
    if (force == NormalGeometrySet && m_bufferGeometry.topLeft() == bufferPosition) {
        return;
    }
    m_bufferGeometry.moveTopLeft(bufferPosition);
    if (areGeometryUpdatesBlocked()) {
        if (pendingGeometryUpdate() == PendingGeometryForced) {
            // Maximum, nothing needed.
        } else if (force == ForceGeometrySet) {
            setPendingGeometryUpdate(PendingGeometryForced);
        } else {
            setPendingGeometryUpdate(PendingGeometryNormal);
        }
        return;
    }
    updateServerGeometry();
    updateWindowRules(Rules::Position);
    screens()->setCurrent(this);
    workspace()->updateStackingOrder();
    // client itself is not damaged
    addRepaintDuringGeometryUpdates();
    updateGeometryBeforeUpdateBlocking();
    emit geometryChanged();
}

bool X11Client::belongToSameApplication(const X11Client *c1, const X11Client *c2, SameApplicationChecks checks)
{
    bool same_app = false;

    // tests that definitely mean they belong together
    if (c1 == c2)
        same_app = true;
    else if (c1->isTransient() && c2->hasTransient(c1, true))
        same_app = true; // c1 has c2 as mainwindow
    else if (c2->isTransient() && c1->hasTransient(c2, true))
        same_app = true; // c2 has c1 as mainwindow
    else if (c1->group() == c2->group())
        same_app = true; // same group
    else if (c1->wmClientLeader() == c2->wmClientLeader()
            && c1->wmClientLeader() != c1->window() // if WM_CLIENT_LEADER is not set, it returns window(),
            && c2->wmClientLeader() != c2->window()) // don't use in this test then
        same_app = true; // same client leader

    // tests that mean they most probably don't belong together
    else if ((c1->pid() != c2->pid() && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses))
            || c1->wmClientMachine(false) != c2->wmClientMachine(false))
        ; // different processes
    else if (c1->wmClientLeader() != c2->wmClientLeader()
            && c1->wmClientLeader() != c1->window() // if WM_CLIENT_LEADER is not set, it returns window(),
            && c2->wmClientLeader() != c2->window() // don't use in this test then
            && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses))
        ; // different client leader
    else if (!resourceMatch(c1, c2))
        ; // different apps
    else if (!sameAppWindowRoleMatch(c1, c2, checks.testFlag(SameApplicationCheck::RelaxedForActive))
            && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses))
        ; // "different" apps
    else if (c1->pid() == 0 || c2->pid() == 0)
        ; // old apps that don't have _NET_WM_PID, consider them different
    // if they weren't found to match above
    else
        same_app = true; // looks like it's the same app

    return same_app;
}

// Non-transient windows with window role containing '#' are always
// considered belonging to different applications (unless
// the window role is exactly the same). KMainWindow sets
// window role this way by default, and different KMainWindow
// usually "are" different application from user's point of view.
// This help with no-focus-stealing for e.g. konqy reusing.
// On the other hand, if one of the windows is active, they are
// considered belonging to the same application. This is for
// the cases when opening new mainwindow directly from the application,
// e.g. 'Open New Window' in konqy ( active_hack == true ).
bool X11Client::sameAppWindowRoleMatch(const X11Client *c1, const X11Client *c2, bool active_hack)
{
    if (c1->isTransient()) {
        while (const X11Client *t = dynamic_cast<const X11Client *>(c1->transientFor()))
            c1 = t;
        if (c1->groupTransient())
            return c1->group() == c2->group();
#if 0
        // if a group transient is in its own group, it didn't possibly have a group,
        // and therefore should be considered belonging to the same app like
        // all other windows from the same app
        || c1->group()->leaderClient() == c1 || c2->group()->leaderClient() == c2;
#endif
    }
    if (c2->isTransient()) {
        while (const X11Client *t = dynamic_cast<const X11Client *>(c2->transientFor()))
            c2 = t;
        if (c2->groupTransient())
            return c1->group() == c2->group();
#if 0
        || c1->group()->leaderClient() == c1 || c2->group()->leaderClient() == c2;
#endif
    }
    int pos1 = c1->windowRole().indexOf('#');
    int pos2 = c2->windowRole().indexOf('#');
    if ((pos1 >= 0 && pos2 >= 0)) {
        if (!active_hack)     // without the active hack for focus stealing prevention,
            return c1 == c2; // different mainwindows are always different apps
        if (!c1->isActive() && !c2->isActive())
            return c1 == c2;
        else
            return true;
    }
    return true;
}

/*

 Transiency stuff: ICCCM 4.1.2.6, NETWM 7.3

 WM_TRANSIENT_FOR is basically means "this is my mainwindow".
 For NET::Unknown windows, transient windows are considered to be NET::Dialog
 windows, for compatibility with non-NETWM clients. KWin may adjust the value
 of this property in some cases (window pointing to itself or creating a loop,
 keeping NET::Splash windows above other windows from the same app, etc.).

 X11Client::transient_for_id is the value of the WM_TRANSIENT_FOR property, after
 possibly being adjusted by KWin. X11Client::transient_for points to the Client
 this Client is transient for, or is NULL. If X11Client::transient_for_id is
 poiting to the root window, the window is considered to be transient
 for the whole window group, as suggested in NETWM 7.3.

 In the case of group transient window, X11Client::transient_for is NULL,
 and X11Client::groupTransient() returns true. Such window is treated as
 if it were transient for every window in its window group that has been
 mapped _before_ it (or, to be exact, was added to the same group before it).
 Otherwise two group transients can create loops, which can lead very very
 nasty things (bug #67914 and all its dupes).

 X11Client::original_transient_for_id is the value of the property, which
 may be different if X11Client::transient_for_id if e.g. forcing NET::Splash
 to be kept on top of its window group, or when the mainwindow is not mapped
 yet, in which case the window is temporarily made group transient,
 and when the mainwindow is mapped, transiency is re-evaluated.

 This can get a bit complicated with with e.g. two Konqueror windows created
 by the same process. They should ideally appear like two independent applications
 to the user. This should be accomplished by all windows in the same process
 having the same window group (needs to be changed in Qt at the moment), and
 using non-group transients poiting to their relevant mainwindow for toolwindows
 etc. KWin should handle both group and non-group transient dialogs well.

 In other words:
 - non-transient windows     : isTransient() == false
 - normal transients         : transientFor() != NULL
 - group transients          : groupTransient() == true

 - list of mainwindows       : mainClients()  (call once and loop over the result)
 - list of transients        : transients()
 - every window in the group : group()->members()
*/

Xcb::TransientFor X11Client::fetchTransient() const
{
    return Xcb::TransientFor(window());
}

void X11Client::readTransientProperty(Xcb::TransientFor &transientFor)
{
    xcb_window_t new_transient_for_id = XCB_WINDOW_NONE;
    if (transientFor.getTransientFor(&new_transient_for_id)) {
        m_originalTransientForId = new_transient_for_id;
        new_transient_for_id = verifyTransientFor(new_transient_for_id, true);
    } else {
        m_originalTransientForId = XCB_WINDOW_NONE;
        new_transient_for_id = verifyTransientFor(XCB_WINDOW_NONE, false);
    }
    setTransient(new_transient_for_id);
}

void X11Client::readTransient()
{
    Xcb::TransientFor transientFor = fetchTransient();
    readTransientProperty(transientFor);
}

void X11Client::setTransient(xcb_window_t new_transient_for_id)
{
    if (new_transient_for_id != m_transientForId) {
        removeFromMainClients();
        X11Client *transient_for = nullptr;
        m_transientForId = new_transient_for_id;
        if (m_transientForId != XCB_WINDOW_NONE && !groupTransient()) {
            transient_for = workspace()->findClient(Predicate::WindowMatch, m_transientForId);
            Q_ASSERT(transient_for != nullptr);   // verifyTransient() had to check this
            transient_for->addTransient(this);
        } // checkGroup() will check 'check_active_modal'
        setTransientFor(transient_for);
        checkGroup(nullptr, true);   // force, because transiency has changed
        workspace()->updateClientLayer(this);
        workspace()->resetUpdateToolWindowsTimer();
        emit transientChanged();
    }
}

void X11Client::removeFromMainClients()
{
    if (transientFor())
        transientFor()->removeTransient(this);
    if (groupTransient()) {
        for (auto it = group()->members().constBegin();
                it != group()->members().constEnd();
                ++it)
            (*it)->removeTransient(this);
    }
}

// *sigh* this transiency handling is madness :(
// This one is called when destroying/releasing a window.
// It makes sure this client is removed from all grouping
// related lists.
void X11Client::cleanGrouping()
{
//    qDebug() << "CLEANGROUPING:" << this;
//    for ( auto it = group()->members().begin();
//         it != group()->members().end();
//         ++it )
//        qDebug() << "CL:" << *it;
//    QList<X11Client *> mains;
//    mains = mainClients();
//    for ( auto it = mains.begin();
//         it != mains.end();
//         ++it )
//        qDebug() << "MN:" << *it;
    removeFromMainClients();
//    qDebug() << "CLEANGROUPING2:" << this;
//    for ( auto it = group()->members().begin();
//         it != group()->members().end();
//         ++it )
//        qDebug() << "CL2:" << *it;
//    mains = mainClients();
//    for ( auto it = mains.begin();
//         it != mains.end();
//         ++it )
//        qDebug() << "MN2:" << *it;
    for (auto it = transients().constBegin();
            it != transients().constEnd();
       ) {
        if ((*it)->transientFor() == this) {
            removeTransient(*it);
            it = transients().constBegin(); // restart, just in case something more has changed with the list
        } else
            ++it;
    }
//    qDebug() << "CLEANGROUPING3:" << this;
//    for ( auto it = group()->members().begin();
//         it != group()->members().end();
//         ++it )
//        qDebug() << "CL3:" << *it;
//    mains = mainClients();
//    for ( auto it = mains.begin();
//         it != mains.end();
//         ++it )
//        qDebug() << "MN3:" << *it;
    // HACK
    // removeFromMainClients() did remove 'this' from transient
    // lists of all group members, but then made windows that
    // were transient for 'this' group transient, which again
    // added 'this' to those transient lists :(
    QList<X11Client *> group_members = group()->members();
    group()->removeMember(this);
    in_group = nullptr;
    for (auto it = group_members.constBegin();
            it != group_members.constEnd();
            ++it)
        (*it)->removeTransient(this);
//    qDebug() << "CLEANGROUPING4:" << this;
//    for ( auto it = group_members.begin();
//         it != group_members.end();
//         ++it )
//        qDebug() << "CL4:" << *it;
    m_transientForId = XCB_WINDOW_NONE;
}

// Make sure that no group transient is considered transient
// for a window that is (directly or indirectly) transient for it
// (including another group transients).
// Non-group transients not causing loops are checked in verifyTransientFor().
void X11Client::checkGroupTransients()
{
    for (auto it1 = group()->members().constBegin();
            it1 != group()->members().constEnd();
            ++it1) {
        if (!(*it1)->groupTransient())  // check all group transients in the group
            continue;                  // TODO optimize to check only the changed ones?
        for (auto it2 = group()->members().constBegin();
                it2 != group()->members().constEnd();
                ++it2) { // group transients can be transient only for others in the group,
            // so don't make them transient for the ones that are transient for it
            if (*it1 == *it2)
                continue;
            for (AbstractClient* cl = (*it2)->transientFor();
                    cl != nullptr;
                    cl = cl->transientFor()) {
                if (cl == *it1) {
                    // don't use removeTransient(), that would modify *it2 too
                    (*it2)->removeTransientFromList(*it1);
                    continue;
                }
            }
            // if *it1 and *it2 are both group transients, and are transient for each other,
            // make only *it2 transient for *it1 (i.e. subwindow), as *it2 came later,
            // and should be therefore on top of *it1
            // TODO This could possibly be optimized, it also requires hasTransient() to check for loops.
            if ((*it2)->groupTransient() && (*it1)->hasTransient(*it2, true) && (*it2)->hasTransient(*it1, true))
                (*it2)->removeTransientFromList(*it1);
            // if there are already windows W1 and W2, W2 being transient for W1, and group transient W3
            // is added, make it transient only for W2, not for W1, because it's already indirectly
            // transient for it - the indirect transiency actually shouldn't break anything,
            // but it can lead to exponentially expensive operations (#95231)
            // TODO this is pretty slow as well
            for (auto it3 = group()->members().constBegin();
                    it3 != group()->members().constEnd();
                    ++it3) {
                if (*it1 == *it2 || *it2 == *it3 || *it1 == *it3)
                    continue;
                if ((*it2)->hasTransient(*it1, false) && (*it3)->hasTransient(*it1, false)) {
                    if ((*it2)->hasTransient(*it3, true))
                        (*it2)->removeTransientFromList(*it1);
                    if ((*it3)->hasTransient(*it2, true))
                        (*it3)->removeTransientFromList(*it1);
                }
            }
        }
    }
}

/**
 * Check that the window is not transient for itself, and similar nonsense.
 */
xcb_window_t X11Client::verifyTransientFor(xcb_window_t new_transient_for, bool set)
{
    xcb_window_t new_property_value = new_transient_for;
    // make sure splashscreens are shown above all their app's windows, even though
    // they're in Normal layer
    if (isSplash() && new_transient_for == XCB_WINDOW_NONE)
        new_transient_for = rootWindow();
    if (new_transient_for == XCB_WINDOW_NONE) {
        if (set)   // sometimes WM_TRANSIENT_FOR is set to None, instead of root window
            new_property_value = new_transient_for = rootWindow();
        else
            return XCB_WINDOW_NONE;
    }
    if (new_transient_for == window()) { // pointing to self
        // also fix the property itself
        qCWarning(KWIN_CORE) << "Client " << this << " has WM_TRANSIENT_FOR poiting to itself." ;
        new_property_value = new_transient_for = rootWindow();
    }
//  The transient_for window may be embedded in another application,
//  so kwin cannot see it. Try to find the managed client for the
//  window and fix the transient_for property if possible.
    xcb_window_t before_search = new_transient_for;
    while (new_transient_for != XCB_WINDOW_NONE
            && new_transient_for != rootWindow()
            && !workspace()->findClient(Predicate::WindowMatch, new_transient_for)) {
        Xcb::Tree tree(new_transient_for);
        if (tree.isNull()) {
            break;
        }
        new_transient_for = tree->parent;
    }
    if (X11Client *new_transient_for_client = workspace()->findClient(Predicate::WindowMatch, new_transient_for)) {
        if (new_transient_for != before_search) {
            qCDebug(KWIN_CORE) << "Client " << this << " has WM_TRANSIENT_FOR poiting to non-toplevel window "
                         << before_search << ", child of " << new_transient_for_client << ", adjusting.";
            new_property_value = new_transient_for; // also fix the property
        }
    } else
        new_transient_for = before_search; // nice try
// loop detection
// group transients cannot cause loops, because they're considered transient only for non-transient
// windows in the group
    int count = 20;
    xcb_window_t loop_pos = new_transient_for;
    while (loop_pos != XCB_WINDOW_NONE && loop_pos != rootWindow()) {
        X11Client *pos = workspace()->findClient(Predicate::WindowMatch, loop_pos);
        if (pos == nullptr)
            break;
        loop_pos = pos->m_transientForId;
        if (--count == 0 || pos == this) {
            qCWarning(KWIN_CORE) << "Client " << this << " caused WM_TRANSIENT_FOR loop." ;
            new_transient_for = rootWindow();
        }
    }
    if (new_transient_for != rootWindow()
            && workspace()->findClient(Predicate::WindowMatch, new_transient_for) == nullptr) {
        // it's transient for a specific window, but that window is not mapped
        new_transient_for = rootWindow();
    }
    if (new_property_value != m_originalTransientForId)
        Xcb::setTransientFor(window(), new_property_value);
    return new_transient_for;
}

void X11Client::addTransient(AbstractClient* cl)
{
    AbstractClient::addTransient(cl);
    if (workspace()->mostRecentlyActivatedClient() == this && cl->isModal())
        check_active_modal = true;
//    qDebug() << "ADDTRANS:" << this << ":" << cl;
//    qDebug() << kBacktrace();
//    for ( auto it = transients_list.begin();
//         it != transients_list.end();
//         ++it )
//        qDebug() << "AT:" << (*it);
}

void X11Client::removeTransient(AbstractClient* cl)
{
//    qDebug() << "REMOVETRANS:" << this << ":" << cl;
//    qDebug() << kBacktrace();
    // cl is transient for this, but this is going away
    // make cl group transient
    AbstractClient::removeTransient(cl);
    if (cl->transientFor() == this) {
        if (X11Client *c = dynamic_cast<X11Client *>(cl)) {
            c->m_transientForId = XCB_WINDOW_NONE;
            c->setTransientFor(nullptr); // SELI
// SELI       cl->setTransient( rootWindow());
            c->setTransient(XCB_WINDOW_NONE);
        }
    }
}

// A new window has been mapped. Check if it's not a mainwindow for this already existing window.
void X11Client::checkTransient(xcb_window_t w)
{
    if (m_originalTransientForId != w)
        return;
    w = verifyTransientFor(w, true);
    setTransient(w);
}

// returns true if cl is the transient_for window for this client,
// or recursively the transient_for window
bool X11Client::hasTransient(const AbstractClient* cl, bool indirect) const
{
    if (const X11Client *c = dynamic_cast<const X11Client *>(cl)) {
        // checkGroupTransients() uses this to break loops, so hasTransient() must detect them
        QList<const X11Client *> set;
        return hasTransientInternal(c, indirect, set);
    }
    return false;
}

bool X11Client::hasTransientInternal(const X11Client *cl, bool indirect, QList<const X11Client *> &set) const
{
    if (const X11Client *t = dynamic_cast<const X11Client *>(cl->transientFor())) {
        if (t == this)
            return true;
        if (!indirect)
            return false;
        if (set.contains(cl))
            return false;
        set.append(cl);
        return hasTransientInternal(t, indirect, set);
    }
    if (!cl->isTransient())
        return false;
    if (group() != cl->group())
        return false;
    // cl is group transient, search from top
    if (transients().contains(const_cast< X11Client *>(cl)))
        return true;
    if (!indirect)
        return false;
    if (set.contains(this))
        return false;
    set.append(this);
    for (auto it = transients().constBegin();
            it != transients().constEnd();
            ++it) {
        const X11Client *c = qobject_cast<const X11Client *>(*it);
        if (!c) {
            continue;
        }
        if (c->hasTransientInternal(cl, indirect, set))
            return true;
    }
    return false;
}

QList<AbstractClient*> X11Client::mainClients() const
{
    if (!isTransient())
        return QList<AbstractClient*>();
    if (const AbstractClient *t = transientFor())
        return QList<AbstractClient*>{const_cast< AbstractClient* >(t)};
    QList<AbstractClient*> result;
    Q_ASSERT(group());
    for (auto it = group()->members().constBegin();
            it != group()->members().constEnd();
            ++it)
        if ((*it)->hasTransient(this, false))
            result.append(*it);
    return result;
}

AbstractClient* X11Client::findModal(bool allow_itself)
{
    for (auto it = transients().constBegin();
            it != transients().constEnd();
            ++it)
        if (AbstractClient* ret = (*it)->findModal(true))
            return ret;
    if (isModal() && allow_itself)
        return this;
    return nullptr;
}

// X11Client::window_group only holds the contents of the hint,
// but it should be used only to find the group, not for anything else
// Argument is only when some specific group needs to be set.
void X11Client::checkGroup(Group* set_group, bool force)
{
    Group* old_group = in_group;
    if (old_group != nullptr)
        old_group->ref(); // turn off automatic deleting
    if (set_group != nullptr) {
        if (set_group != in_group) {
            if (in_group != nullptr)
                in_group->removeMember(this);
            in_group = set_group;
            in_group->addMember(this);
        }
    } else if (info->groupLeader() != XCB_WINDOW_NONE) {
        Group* new_group = workspace()->findGroup(info->groupLeader());
        X11Client *t = qobject_cast<X11Client *>(transientFor());
        if (t != nullptr && t->group() != new_group) {
            // move the window to the right group (e.g. a dialog provided
            // by different app, but transient for this one, so make it part of that group)
            new_group = t->group();
        }
        if (new_group == nullptr)   // doesn't exist yet
            new_group = new Group(info->groupLeader());
        if (new_group != in_group) {
            if (in_group != nullptr)
                in_group->removeMember(this);
            in_group = new_group;
            in_group->addMember(this);
        }
    } else {
        if (X11Client *t = qobject_cast<X11Client *>(transientFor())) {
            // doesn't have window group set, but is transient for something
            // so make it part of that group
            Group* new_group = t->group();
            if (new_group != in_group) {
                if (in_group != nullptr)
                    in_group->removeMember(this);
                in_group = t->group();
                in_group->addMember(this);
            }
        } else if (groupTransient()) {
            // group transient which actually doesn't have a group :(
            // try creating group with other windows with the same client leader
            Group* new_group = workspace()->findClientLeaderGroup(this);
            if (new_group == nullptr)
                new_group = new Group(XCB_WINDOW_NONE);
            if (new_group != in_group) {
                if (in_group != nullptr)
                    in_group->removeMember(this);
                in_group = new_group;
                in_group->addMember(this);
            }
        } else { // Not transient without a group, put it in its client leader group.
            // This might be stupid if grouping was used for e.g. taskbar grouping
            // or minimizing together the whole group, but as long as it is used
            // only for dialogs it's better to keep windows from one app in one group.
            Group* new_group = workspace()->findClientLeaderGroup(this);
            if (in_group != nullptr && in_group != new_group) {
                in_group->removeMember(this);
                in_group = nullptr;
            }
            if (new_group == nullptr)
                new_group = new Group(XCB_WINDOW_NONE);
            if (in_group != new_group) {
                in_group = new_group;
                in_group->addMember(this);
            }
        }
    }
    if (in_group != old_group || force) {
        for (auto it = transients().constBegin();
                it != transients().constEnd();
           ) {
            auto *c = *it;
            // group transients in the old group are no longer transient for it
            if (c->groupTransient() && c->group() != group()) {
                removeTransientFromList(c);
                it = transients().constBegin(); // restart, just in case something more has changed with the list
            } else
                ++it;
        }
        if (groupTransient()) {
            // no longer transient for ones in the old group
            if (old_group != nullptr) {
                for (auto it = old_group->members().constBegin();
                        it != old_group->members().constEnd();
                        ++it)
                    (*it)->removeTransient(this);
            }
            // and make transient for all in the new group
            for (auto it = group()->members().constBegin();
                    it != group()->members().constEnd();
                    ++it) {
                if (*it == this)
                    break; // this means the window is only transient for windows mapped before it
                (*it)->addTransient(this);
            }
        }
        // group transient splashscreens should be transient even for windows
        // in group mapped later
        for (auto it = group()->members().constBegin();
                it != group()->members().constEnd();
                ++it) {
            if (!(*it)->isSplash())
                continue;
            if (!(*it)->groupTransient())
                continue;
            if (*it == this || hasTransient(*it, true))    // TODO indirect?
                continue;
            addTransient(*it);
        }
    }
    if (old_group != nullptr)
        old_group->deref(); // can be now deleted if empty
    checkGroupTransients();
    checkActiveModal();
    workspace()->updateClientLayer(this);
}

// used by Workspace::findClientLeaderGroup()
void X11Client::changeClientLeaderGroup(Group* gr)
{
    // transientFor() != NULL are in the group of their mainwindow, so keep them there
    if (transientFor() != nullptr)
        return;
    // also don't change the group for window which have group set
    if (info->groupLeader())
        return;
    checkGroup(gr);   // change group
}

bool X11Client::check_active_modal = false;

void X11Client::checkActiveModal()
{
    // if the active window got new modal transient, activate it.
    // cannot be done in AddTransient(), because there may temporarily
    // exist loops, breaking findModal
    X11Client *check_modal = dynamic_cast<X11Client *>(workspace()->mostRecentlyActivatedClient());
    if (check_modal != nullptr && check_modal->check_active_modal) {
        X11Client *new_modal = dynamic_cast<X11Client *>(check_modal->findModal());
        if (new_modal != nullptr && new_modal != check_modal) {
            if (!new_modal->isManaged())
                return; // postpone check until end of manage()
            workspace()->activateClient(new_modal);
        }
        check_modal->check_active_modal = false;
    }
}

} // namespace

