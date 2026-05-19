#include "PagePanel.h"
#include "PagePanelListView.h"
#include "../PageThumbnailModel.h"
#include "../PageThumbnailDelegate.h"
#include "../ThumbnailRenderer.h"
#include "../../core/Document.h"
#include <QVBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <QResizeEvent>

// ============================================================================
// Constructor / Destructor
// ============================================================================

PagePanel::PagePanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    setupConnections();
}

PagePanel::~PagePanel()
{
    // Children are parented, will be deleted automatically
}

// ============================================================================
// Setup
// ============================================================================

void PagePanel::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Create model
    m_model = new PageThumbnailModel(this);
    
    // Create delegate
    m_delegate = new PageThumbnailDelegate(this);
    
    // Create list view (custom class with long-press drag support)
    m_listView = new PagePanelListView(this);
    configureListView();
    
    // Set model and delegate
    m_listView->setModel(m_model);
    m_listView->setItemDelegate(m_delegate);
    
    layout->addWidget(m_listView);
    
    // Create invalidation timer
    m_invalidationTimer = new QTimer(this);
    m_invalidationTimer->setSingleShot(true);
    m_invalidationTimer->setInterval(INVALIDATION_DELAY_MS);
    
    // Create resize debounce timer
    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(RESIZE_DEBOUNCE_MS);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingThumbnailWidth > 0) {
            m_model->setThumbnailWidth(m_pendingThumbnailWidth);
            m_pendingThumbnailWidth = 0;
        }
        
        // After the resize has settled, re-center on the current page if it
        // is no longer visible. Uses the same offscreen-only policy as
        // onCurrentPageChanged() so we never steal the scroll position when
        // the user is intentionally browsing the thumbnail list.
        if (m_document && m_currentPageIndex >= 0 && m_listView) {
            QModelIndex idx = m_model->index(m_currentPageIndex, 0);
            if (idx.isValid()) {
                QRect itemRect = m_listView->visualRect(idx);
                QRect viewRect = m_listView->viewport()->rect();
                if (!viewRect.intersects(itemRect)) {
                    scrollToCurrentPage();
                }
            }
        }
    });
    
    // Apply initial theme
    applyTheme();
}

void PagePanel::configureListView()
{
    // Basic configuration
    m_listView->setViewMode(QListView::ListMode);
    m_listView->setFlow(QListView::TopToBottom);
    m_listView->setWrapping(false);
    m_listView->setResizeMode(QListView::Adjust);
    // DEBUG: Disabled batched mode - was possibly causing scroll jumps
    // m_listView->setLayoutMode(QListView::Batched);
    // m_listView->setBatchSize(10);
    m_listView->setLayoutMode(QListView::SinglePass);
    
    // Selection
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setSelectionBehavior(QAbstractItemView::SelectRows);
    
    // Scrolling
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    
    // Drag and drop
    m_listView->setDragEnabled(true);
    m_listView->setAcceptDrops(true);
    m_listView->setDropIndicatorShown(true);
    m_listView->setDragDropMode(QAbstractItemView::InternalMove);
    m_listView->setDefaultDropAction(Qt::MoveAction);
    
    // Appearance
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setSpacing(0);
    m_listView->setUniformItemSizes(false);  // Items may have different heights
    
    // Enable mouse tracking for hover effects
    m_listView->setMouseTracking(true);
    m_listView->viewport()->setMouseTracking(true);
    m_listView->setAttribute(Qt::WA_Hover, true);
    m_listView->viewport()->setAttribute(Qt::WA_Hover, true);
    
    // Note: Touch scrolling is handled manually by PagePanelListView
}

void PagePanel::setupConnections()
{
    // Item click
    connect(m_listView, &QListView::clicked, this, &PagePanel::onItemClicked);
    
    // Long-press drag request (touch input)
    connect(m_listView, &PagePanelListView::dragRequested,
            this, &PagePanel::onDragRequested);
    
    // Page dropped from model
    connect(m_model, &PageThumbnailModel::pageDropped, 
            this, &PagePanel::onModelPageDropped);
    
    // Invalidation timer
    connect(m_invalidationTimer, &QTimer::timeout, 
            this, &PagePanel::performPendingInvalidation);
}

// ============================================================================
// Document Binding
// ============================================================================

void PagePanel::setDocument(Document* doc)
{
    if (m_document == doc) {
        return;
    }
    
    m_document = doc;
    m_currentPageIndex = 0;
    
    // Update model
    m_model->setDocument(doc);
    m_model->setCurrentPageIndex(0);
    
    // setDocument() performs a begin/endResetModel like onPageCountChanged.
    // Force-reapply the current layout mode so 2-column users don't get
    // collapsed back to 1-column when switching documents in the same tab.
    applyLayoutMode(m_currentColumns, /*force=*/true);
    
    // Update thumbnail width based on current size
    updateThumbnailWidth();
    
    // Clear pending invalidations
    m_pendingInvalidations.clear();
    m_needsFullRefresh = false;
}

// ============================================================================
// Current Page
// ============================================================================

void PagePanel::setCurrentPageIndex(int index)
{
    if (m_currentPageIndex != index && m_document) {
        m_currentPageIndex = index;
        m_model->setCurrentPageIndex(index);
    }
}

void PagePanel::onCurrentPageChanged(int pageIndex)
{
    int previousPage = m_currentPageIndex;
    setCurrentPageIndex(pageIndex);
    
    // Only auto-scroll if the page change is significant (not just minor viewport scroll)
    // and if the new current page is not already visible in the list view
    if (isVisible() && previousPage != pageIndex) {
        // Check if the current page item is already visible
        QModelIndex index = m_model->index(pageIndex, 0);
        if (index.isValid()) {
            QRect itemRect = m_listView->visualRect(index);
            QRect viewRect = m_listView->viewport()->rect();
            
            // Only scroll if item is completely outside visible area
            if (!viewRect.intersects(itemRect)) {
                scrollToCurrentPage();
            }
        }
    }
}

void PagePanel::scrollToCurrentPage()
{
    if (!m_document || m_currentPageIndex < 0) {
        return;
    }
    
    QModelIndex index = m_model->index(m_currentPageIndex, 0);
    if (index.isValid()) {
        m_listView->scrollTo(index, QAbstractItemView::EnsureVisible);
    }
}

// ============================================================================
// Scroll Position State
// ============================================================================

int PagePanel::scrollPosition() const
{
    return m_listView->verticalScrollBar()->value();
}

void PagePanel::setScrollPosition(int pos)
{
    m_listView->verticalScrollBar()->setValue(pos);
}

void PagePanel::saveTabState(int tabIndex)
{
    m_tabScrollPositions[tabIndex] = scrollPosition();
}

void PagePanel::restoreTabState(int tabIndex)
{
    if (m_tabScrollPositions.contains(tabIndex)) {
        setScrollPosition(m_tabScrollPositions.value(tabIndex));
    } else {
        // New tab - scroll to current page
        scrollToCurrentPage();
    }
}

void PagePanel::clearTabState(int tabIndex)
{
    m_tabScrollPositions.remove(tabIndex);
}

// ============================================================================
// Theme
// ============================================================================

void PagePanel::setDarkMode(bool dark)
{
    if (m_darkMode != dark) {
        m_darkMode = dark;
        m_delegate->setDarkMode(dark);
        applyTheme();
        m_listView->viewport()->update();
    }
}

void PagePanel::setPdfDarkMode(bool enabled)
{
    if (m_model) {
        m_model->setPdfDarkMode(enabled);
    }
}

void PagePanel::applyTheme()
{
    // Unified gray colors: dark #2a2e32, light #F5F5F5
    QString bgColor = m_darkMode ? "#2a2e32" : "#F5F5F5";
    
    m_listView->setStyleSheet(QString(R"(
        QListView {
            background-color: %1;
            border: none;
            outline: none;
        }
        QListView::item {
            border: none;
            padding: 0px;
        }
        QListView::item:selected {
            background-color: transparent;
        }
    )").arg(bgColor));
}

// ============================================================================
// Thumbnail Access
// ============================================================================

QPixmap PagePanel::thumbnailForPage(int pageIndex) const
{
    if (!m_model) {
        return QPixmap();
    }
    return m_model->thumbnailForPage(pageIndex);
}

// ============================================================================
// Thumbnail Invalidation
// ============================================================================

void PagePanel::invalidateThumbnail(int pageIndex)
{
    // Optimization: If panel is not visible, just mark for refresh when it becomes visible.
    // This avoids clearing cached thumbnails unnecessarily while the user is editing
    // on another sidebar tab, and prevents any rendering work until the panel is shown.
    if (!isVisible()) {
        m_pendingInvalidations.insert(pageIndex);
        // Don't start the timer - we'll handle it in showEvent
        return;
    }
    
    m_pendingInvalidations.insert(pageIndex);
    
    // Start debounce timer if not already running
    if (!m_invalidationTimer->isActive()) {
        m_invalidationTimer->start();
    }
}

void PagePanel::invalidateAllThumbnails()
{
    m_needsFullRefresh = true;
    m_pendingInvalidations.clear();
    
    // Start debounce timer if not already running
    if (!m_invalidationTimer->isActive()) {
        m_invalidationTimer->start();
    }
}

void PagePanel::cancelPendingRenders()
{
    // Cancel all pending thumbnail renders and wait for completion.
    // This is used before operations that access Document pages directly
    // (like MainWindow::renderPage0Thumbnail) to avoid race conditions.
    if (m_model) {
        m_model->cancelPendingRenders();
    }
}

void PagePanel::performPendingInvalidation()
{
    if (m_needsFullRefresh) {
        m_model->invalidateAllThumbnails();
        m_needsFullRefresh = false;
    } else {
        for (int pageIndex : m_pendingInvalidations) {
            m_model->invalidateThumbnail(pageIndex);
        }
    }
    
    m_pendingInvalidations.clear();
}

// ============================================================================
// Page Count Change
// ============================================================================

void PagePanel::onPageCountChanged()
{
    m_model->onPageCountChanged();
    
    // After a full model reset, QListView's internal wrap-mode state can be
    // disrupted with setUniformItemSizes(false) and silently fall back to a
    // single column. Force-reapply the current layout mode so 2-column users
    // don't snap back to 1-column when adding/inserting/deleting a page.
    applyLayoutMode(m_currentColumns, /*force=*/true);
    
    // Update thumbnail width in case layout changed
    updateThumbnailWidth();
}

// ============================================================================
// Private Slots
// ============================================================================

void PagePanel::onItemClicked(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }
    
    // Note: With manual touch scrolling in PagePanelListView,
    // clicked() is only emitted for taps, not during scrolling
    
    // Only respond to clicks within the thumbnail region (not the frame/padding)
    // This makes it easier to scroll without accidentally switching pages
    QPoint clickPos = m_listView->lastPressPosition();
    QRect itemRect = m_listView->visualRect(index);
    
    // Get aspect ratio for this specific page
    qreal aspectRatio = index.data(PageThumbnailModel::PageAspectRatioRole).toReal();
    if (aspectRatio <= 0) {
        aspectRatio = -1;  // Use delegate's default
    }
    
    QRect thumbRect = m_delegate->thumbnailRect(itemRect, aspectRatio);
    
    if (!thumbRect.contains(clickPos)) {
        return;  // Click was outside thumbnail - ignore (allow scrolling in padding area)
    }
    
    int pageIndex = index.data(PageThumbnailModel::PageIndexRole).toInt();
    emit pageClicked(pageIndex);
}

void PagePanel::onModelPageDropped(int fromIndex, int toIndex)
{
    // Forward the signal
    emit pageDropped(fromIndex, toIndex);
}

void PagePanel::onDragRequested(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }
    
    // Start drag operation (triggered by long-press on touch)
    m_listView->beginDrag(Qt::MoveAction);
}

// ============================================================================
// Thumbnail Width
// ============================================================================

int PagePanel::chooseColumnCount(int panelWidth) const
{
    // Treat non-positive widths (hidden / not yet sized) as "keep current mode"
    // so we don't accidentally collapse to 1-col on startup before the splitter
    // has assigned a real size.
    if (panelWidth <= 0) {
        return m_currentColumns;
    }
    
    if (panelWidth >= TWO_COL_ENTER_WIDTH) {
        return 2;
    }
    if (panelWidth <= TWO_COL_EXIT_WIDTH) {
        return 1;
    }
    // Inside the hysteresis band: keep current mode.
    return m_currentColumns;
}

void PagePanel::applyLayoutMode(int columns, bool force)
{
    // Allow callers to force a re-application even when columns are unchanged.
    // This is needed after a full model reset (begin/endResetModel), where
    // QListView's internal layout state can lose the wrap-mode bookkeeping
    // (especially with setUniformItemSizes(false)) and silently fall back to
    // a single column even though we never asked it to.
    if (columns == m_currentColumns && !force) {
        return;
    }
    m_currentColumns = columns;
    
    if (columns >= 2) {
        // Items flow left-to-right and wrap to the next row once a row is full.
        // Combined with a per-item width hint of roughly viewport/2 this gives
        // a clean 2-column grid while letting QListView handle the per-row
        // height (which may vary if pages have mixed aspect ratios).
        m_listView->setFlow(QListView::LeftToRight);
        m_listView->setWrapping(true);
    } else {
        // Classic vertical list: one item per row, no wrapping.
        m_listView->setFlow(QListView::TopToBottom);
        m_listView->setWrapping(false);
    }
    
    // Force an immediate relayout instead of waiting for the next paint.
    m_listView->doItemsLayout();
    
    // After the new layout has been computed, re-center on the current page
    // so the user doesn't get lost when columns change. Defer to the event
    // loop so visualRect() reflects the new layout.
    QTimer::singleShot(0, this, [this]() {
        scrollToCurrentPage();
    });
}

void PagePanel::updateThumbnailWidth()
{
    // Use viewport width (excludes vertical scrollbar) for accurate per-column
    // sizing. Only trust the viewport when the panel is actually visible:
    // while hidden inside an inactive tab, the QListView's viewport is not
    // yet laid out for the current geometry, so its width can come back
    // unreliably small. If we used that small value, the 2-column formula
    // would clamp the delegate to MIN_THUMBNAIL_WIDTH and bake a stale
    // sizeHint into doItemsLayout, producing a 3+ column layout the first
    // time the user opens the tab. Fall back to widget width minus the
    // platform's vertical-scrollbar reservation, which matches what the
    // viewport will report once the tab is shown.
    int viewportWidth = 0;
    if (isVisible() && m_listView && m_listView->viewport()) {
        viewportWidth = m_listView->viewport()->width();
    }
    if (viewportWidth <= 0) {
        int scrollbarWidth = m_listView
            ? m_listView->verticalScrollBar()->sizeHint().width()
            : 18;
        viewportWidth = qMax(0, width() - scrollbarWidth);
    }
    if (viewportWidth <= 0) {
        viewportWidth = width();
    }
    
    const int panelWidth = width();
    const int columns = chooseColumnCount(panelWidth);
    
    int thumbnailWidth;
    if (columns >= 2) {
        const int available = viewportWidth - THUMBNAIL_PADDING * 2 - COLUMN_GAP;
        thumbnailWidth = qMax(MIN_THUMBNAIL_WIDTH, available / 2);
    } else {
        const int available = viewportWidth - THUMBNAIL_PADDING * 2;
        thumbnailWidth = qMax(MIN_THUMBNAIL_WIDTH, available);
    }
    
    qreal dpr = devicePixelRatioF();
    
    // Update delegate FIRST so the next doItemsLayout (inside applyLayoutMode)
    // uses the new per-cell sizeHint. Otherwise Qt's wrap calculation would
    // see oversized items from the previous mode and pack them 1-per-row,
    // leaving thumbnails small but still in a single column.
    m_delegate->setThumbnailWidth(thumbnailWidth);
    
    // Flip layout mode after the delegate width has been updated.
    if (columns != m_currentColumns) {
        applyLayoutMode(columns);
    }
    
    // Debounce the heavy model update (cancels renders + clears cache + re-requests)
    m_pendingThumbnailWidth = thumbnailWidth;
    m_model->setDevicePixelRatio(dpr);
    m_resizeDebounceTimer->start();
}

// ============================================================================
// Event Handlers
// ============================================================================

// Override resizeEvent to update thumbnail width
void PagePanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateThumbnailWidth();
}

// Override showEvent to handle refresh when becoming visible
void PagePanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    
    // Process any pending invalidations that accumulated while hidden
    // This is more efficient than clearing cache while hidden (thumbnails stay cached)
    if (m_needsFullRefresh) {
        m_model->invalidateAllThumbnails();
        m_needsFullRefresh = false;
        m_pendingInvalidations.clear();  // Full refresh supersedes individual invalidations
    } else if (!m_pendingInvalidations.isEmpty()) {
        // Invalidate only the pages that were modified while hidden
        for (int pageIndex : m_pendingInvalidations) {
            m_model->invalidateThumbnail(pageIndex);
        }
        m_pendingInvalidations.clear();
    }
    
    // The panel may have been resized while hidden (e.g., user widened the
    // sidebar from a different tab). In that case the delegate's sizeHint
    // and the QListView's wrap layout can be stale because the viewport
    // width was unreliable while hidden. Refresh the width now that the
    // viewport is properly laid out, then force a re-layout so the first
    // visible frame uses the correct per-cell sizeHint.
    //
    // updateThumbnailWidth() only flips columns when the column count
    // changes, so it cannot by itself force Qt to re-wrap when only the
    // per-cell sizeHint changed. The force-true call below covers that
    // case (and harmlessly re-applies flow/wrap when nothing changed).
    updateThumbnailWidth();
    applyLayoutMode(m_currentColumns, /*force=*/true);
    
    // Only scroll to current page on initial show, not every show
    // The user's scroll position should be preserved
    // scrollToCurrentPage();  // Disabled - was causing scroll jumps
}

