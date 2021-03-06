//
//  QtListWidget.cpp
//  MusicPlayer
//
//  Created by Albert Zeyer on 21.01.14.
//  Copyright (c) 2014 Albert Zeyer. All rights reserved.
//

#include "QtListWidget.hpp"
#include "PythonHelpers.h"
#include "Builders.hpp"
#include "FunctionWrapper.hpp"
#include "QtUtils.hpp"
#include <vector>
#include <string>
#include <QApplication>
#include <QPainter>

// Possible implementations:
// - QScrollArea (all by myself)
// - http://www.qtcentre.org/threads/27777-Customize-QListWidgetItem-how-to
// - http://qt-project.org/doc/qt-4.8/itemviews-stardelegate.html
// - http://qt-project.org/doc/qt-4.8/qabstractitemdelegate.html
// - QListWidget::setItemWidget (too slow?)

RegisterControl(List);

struct ListItem {
	PyObject* subjectObject; // XXX: must be weak
	PyQtGuiObject* control;

	ListItem(PyObject* obj)
		: subjectObject(obj), // we expect to already have increfd
		  control(NULL)
	{}
	~ListItem() {
		Py_CLEAR(control);
	}

	void setupControl(int width, PyQtGuiObject* parent) {
		assert(subjectObject);

		PyScopedGIL gil;

		if(!control) {
			control = guiQt_createControlObject(subjectObject, parent);
			if(!control) return; // XXX err

			// XXX
			//control->PresetSize.x = [scrollview contentSize].width;
			//if(!guiObjectList.empty())
			//	subCtr->PresetSize.y = guiObjectList[0]->get_size(guiObjectList[0]).y;

			if(!_buildControlObject_pre(control)) return; // XXX err
			// XXX: handle size?
			if(!_buildControlObject_post(control)) return; // XXX err

		}

		int h = control->get_size(control).y;
		control->set_size(control, Vec(width, h));

		control->layout();
	}
};

class QtListWidget::ListModel : public QAbstractItemModel {
private:
	QtBaseWidget::WeakRef listWidget;
	std::vector<ListItem*> items;

public:
	ListModel(QtListWidget& owner)
		: listWidget(owner)
	{}

	~ListModel() {
		for(ListItem* item : items)
			delete item;
		items.clear();
	}

	virtual QModelIndex index(int row, int column, const QModelIndex &parent) const {
		if(column > 0) return QModelIndex();
		if(parent.isValid()) return QModelIndex();
		return createIndex(row, column);
	}

	virtual QModelIndex parent(const QModelIndex& /*child*/) const {
		return QModelIndex(); // always invalid
	}

	virtual int rowCount(const QModelIndex &parent) const {
		if(parent.isValid()) return 0;
		return (int) items.size();
	}

	virtual int columnCount(const QModelIndex &parent) const {
		if(parent.isValid()) return 0;
		return 1; // only one row
	}

	virtual QVariant data(const QModelIndex &index, int role) const {
		if(index.column() != 0) return QVariant();
		if(role != Qt::DisplayRole) return QVariant();
		int idx = index.row();
		if(idx < 0) return QVariant();
		if((size_t)idx >= items.size()) return QVariant();
		return QVariant::fromValue((void*) items[idx]);
	}

	void controlChildIter(QtBaseWidget::ChildIterCallback cb) {
		for(ListItem* item : items) {
			bool stop = false;
			if(item->control)
				cb(item->control, stop);
			if(stop) break;
		}
	}

	void push_back(PyObject* value) {
		dispatch_sync_main_queue([=]() {
			int idx = items.size();
			beginInsertRows(QModelIndex(), idx, idx);
			items.push_back(new ListItem(value));
			endInsertRows();
		});
	}

	void insert(int _idx, PyObject* value) {
		dispatch_sync_main_queue([=]() {
			int idx = _idx;
			if(idx < 0) idx = 0;
			if((size_t)idx > items.size()) idx = items.size();
			beginInsertRows(QModelIndex(), idx, idx);
			items.insert(items.begin() + idx, new ListItem(value));
			endInsertRows();
		});
	}

	void remove(int idx) {
		dispatch_sync_main_queue([=]() {
			if(idx < 0) return;
			if((size_t)idx >= items.size()) return;
			beginRemoveRows(QModelIndex(), idx, idx);
			ListItem* item = items[idx];
			items.erase(items.begin() + idx);
			delete item;
			endRemoveRows();
		});
	}

	void clear() {
		dispatch_sync_main_queue([=]() {
			beginResetModel();
			for(ListItem* item : items)
				delete item;
			items.clear();
			endResetModel();
		});
	}

	void updateLayout() {
		if(items.empty()) return;
		emit dataChanged(createIndex(0,0), createIndex(items.size()-1, 0));
	}

	QtBaseWidget::WeakRef getFirstWidget() const {
		for(ListItem* item : items) {
			PyQtGuiObject* control = item->control;
			if(control) return control->widget;
		}
		return QtBaseWidget::WeakRef();
	}

	const QtBaseWidget::WeakRef& getListWidget() const {
		return listWidget;
	}

	int getOwnerWidth() const {
		QtBaseWidget::ScopedRef owner(listWidget);
		if(!owner) return 200; // fallback
		QtListWidget* widget = dynamic_cast<QtListWidget*>(owner.get());
		assert(widget);
		return widget->size().width();
	}

	QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const {
		if(index.column() != 0) return QSize();

		/*
		{
			QtBaseWidget::ScopedRef widget(getFirstWidget());
			if(widget) return widget->size();
		}
		*/

		// fallback
		return QSize(getOwnerWidth() - 2, 22);
	}

};

class QtListWidget::ItemDelegate : public QAbstractItemDelegate {
	ListModel* listModel;

public:
	ItemDelegate(ListModel* m) : listModel(m) {}

	virtual void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
		ListItem* item = (ListItem*) index.data().value<void*>();
		if(!item) goto error;

		painter->setOpacity(1);
		if(option.state & QStyle::State_Selected)
			painter->fillRect(option.rect, option.palette.highlight());
		else if(option.features & QStyleOptionViewItem::Alternate)
			painter->fillRect(option.rect, option.palette.alternateBase());

		{
			QtBaseWidget::ScopedRef listWidget(listModel->getListWidget());
			if(!listWidget) goto error;
			PyQtGuiObject* parent = listWidget->getControl();
			if(!parent) goto error;
			item->setupControl(listWidget->size().width(), parent);
		}

		{
			if(!item->control) goto error;
			QtBaseWidget::ScopedRef widget(item->control->widget);
			if(!widget) goto error;

			painter->setOpacity(0.8); // XXX?
			widget->setAutoFillBackground(false);
			widget->render(
				painter,
				QPoint(option.rect.x(), option.rect.y()),
				QRegion(0, 0, option.rect.width(), option.rect.height()),
				QWidget::DrawChildren);
		}

		return;

	error:
		painter->fillRect(option.rect, option.palette.dark());
	}

	virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
		return listModel->sizeHint(option, index);
	}
};

class QtListWidget::ListView : public QListView {
public:
	ListView(QtListWidget& parent) : QListView(&parent) {
		setUniformItemSizes(true);
		setAlternatingRowColors(true);
		//setBatchSize(1);
		//setLayoutMode(QListView::Batched);
		setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	}
};

QtListWidget::QtListWidget(PyQtGuiObject* control)
	: QtBaseWidget(control),
	  listWidget(NULL),
	  subjectListRef(NULL),
	  autoScrolldown(false)
{
	resize(width(), /* default height */ 80);

	listModel = new ListModel(*this);

	listWidget = new ListView(*this);
	listWidget->setItemDelegate(new ItemDelegate(listModel));
	listWidget->setModel(listModel);
	listWidget->resize(size());
	listWidget->show();


	{
		PyScopedGIL gil;

		control->OuterSpace = Vec(0,0);

		autoScrolldown = attrChain_bool_default(control->attr, "autoScrolldown", false);

		{
			PyObject* handler = attrChain(control->attr, "dragHandler");
			if(!handler) {
				printf("Qt ListControl: error while getting control.attr.dragHandler\n");
				if(PyErr_Occurred())
					PyErr_Print();
			}
			/*
			if(handler) {
				if(handler != Py_None) {
					dragHandlerRef = (PyWeakReference*) PyWeakref_NewRef(handler, NULL);
					if(!dragHandlerRef) {
						printf("Qt ListControl: cannot create dragHandlerRef\n");
						if(PyErr_Occurred())
							PyErr_Print();
					}
				}
				Py_CLEAR(handler);
			}
			*/
		}

		if(!control->subjectObject) {
			printf("Qt ListControl: subjectObject is NULL\n");
		} else {
			subjectListRef = (PyWeakReference*) PyWeakref_NewRef(control->subjectObject, NULL);
			if(!subjectListRef) {
				printf("Qt ListControl: cannot create subjectListRef\n");
				goto finalPyInit;
			}
		}

	finalPyInit:
		if(PyErr_Occurred()) PyErr_Print();
	}

	if(!subjectListRef) return;

	/*
	if(dragHandlerRef) {
		[self registerForDraggedTypes:@[NSFilenamesPboardType]];
		dragCursor = [[_NSFlippedView alloc] initWithFrame:NSMakeRect(0,0,[scrollview contentSize].width,2)];
		[dragCursor setAutoresizingMask:NSViewWidthSizable];
		[dragCursor setBackgroundColor:[NSColor blackColor]];
		[documentView addSubview:dragCursor];
	}
	*/

	WeakRef selfWeakRef(*this);

	// do initial fill in background
	dispatch_async_background_queue([selfWeakRef]() {
		PyScopedGIL gil;

		PyObject* lock = NULL;
		PyObject* lockEnterRes = NULL;
		PyObject* lockExitRes = NULL;
		PyObject* list = NULL;

		ScopedRef selfWeakRefScope(selfWeakRef);
		QtListWidget* self = (QtListWidget*) selfWeakRefScope.get();

		if(!self) goto finalInitialFill;

		list = PyWeakref_GET_OBJECT(self->subjectListRef);
		if(!list) goto finalInitialFill;
		Py_INCREF(list);

		// We must lock the list.lock because we must ensure that we have the
		// data in sync.
		lock = PyObject_GetAttrString(list, "lock");
		if(!lock) {
			printf("Qt ListControl: list.lock not found\n");
			if(PyErr_Occurred())
				PyErr_Print();
			goto finalInitialFill;
		}

		lockEnterRes = PyObject_CallMethod(lock, (char*)"__enter__", NULL);
		if(!lockEnterRes) {
			printf("Qt ListControl: list.lock.__enter__ failed\n");
			if(PyErr_Occurred())
				PyErr_Print();
			goto finalInitialFill;
		}

		{
			PyObject* listIter = PyObject_GetIter(list);
			if(!listIter) {
				printf("Qt ListControl: cannot get iter(list)\n");
				if(PyErr_Occurred())
					PyErr_Print();
				goto finalInitialFill;
			}

			while(true) {
				PyObject* listIterItem = PyIter_Next(listIter);
				if(listIterItem == NULL) break;
				self->listModel->push_back(listIterItem /* overtake ownership */);
			}

			if(PyErr_Occurred()) {
				printf("Qt ListControl: error while copying list\n");
				PyErr_Print();
			}
			Py_DECREF(listIter);
		}

		if(self->autoScrolldown)
			self->listWidget->scrollToBottom();

		// We expect the list ( = control->subjectObject ) to support a certain interface,
		// esp. to have onInsert, onRemove and onClear as utils.Event().
		{
			auto registerEv = [=](const char* evName, PyCallback func) {
				PyQtGuiObject* control = NULL;
				PyObject* event = NULL;
				PyObject* res = NULL;
				PyObject* callbackWrapper = NULL;

				control = self->getControl();
				if(!control) goto finalRegisterEv;

				callbackWrapper = (PyObject*) newFunctionWrapper(func);
				if(!callbackWrapper) {
					printf("Qt ListControl: cannot create callback wrapper for %s\n", evName);
					goto finalRegisterEv;
				}

				event = PyObject_GetAttrString(list, evName);
				if(!event) {
					printf("Qt ListControl: cannot get list event for %s\n", evName);
					goto finalRegisterEv;
				}

				res = PyObject_CallMethod(event, (char*)"register", (char*)"(O)", callbackWrapper);
				if(!res) {
					printf("Qt ListControl: cannot register list event callback for %s\n", evName);
					goto finalRegisterEv;
				}

				// And we assign the wrapper-object to the CocoaGuiObject(control),
				// otherwise there would not be any strong ref to it.
				// We do this as the last step, so that we do it only if we did not fail so far.
				char attribName[255];
				snprintf(attribName, sizeof(attribName), "_%s", evName);
				if(PyObject_SetAttrString((PyObject*) control, attribName, callbackWrapper) != 0) {
					printf("Qt ListControl: failed to set %s\n", attribName);
					goto finalRegisterEv;
				}

			finalRegisterEv:
				if(PyErr_Occurred())
					PyErr_Print();
				Py_XDECREF(control);
				Py_XDECREF(res);
				Py_XDECREF(event);
				Py_XDECREF(callbackWrapper);
			};

			registerEv("onInsert", [selfWeakRef](PyObject* args, PyObject* kws) {
				ScopedRef selfWeakRefScope(selfWeakRef);
				QtListWidget* self = (QtListWidget*) selfWeakRefScope.get();
				if(self) {
					int idx; PyObject* v;
					static const char *kwlist[] = {"index", "value", NULL};
					if(!PyArg_ParseTupleAndKeywords(args, kws, "iO:onInsert", (char**)kwlist, &idx, &v))
						return (PyObject*) NULL;
					Py_INCREF(v);
					self->listModel->insert(idx, v /* overtake */);
					if(self->autoScrolldown) {
						dispatch_sync_main_queue([selfWeakRef]() {
							ScopedRef selfWeakRefScope(selfWeakRef);
							QtListWidget* self = (QtListWidget*) selfWeakRefScope.get();
							if(self && self->autoScrolldown)
								self->listWidget->scrollToBottom();
						});
					}
				}
				Py_INCREF(Py_None);
				return Py_None;
			});
			registerEv("onRemove", [selfWeakRef](PyObject* args, PyObject* kws) {
				ScopedRef selfWeakRefScope(selfWeakRef);
				QtListWidget* self = (QtListWidget*) selfWeakRefScope.get();
				if(self) {
					int idx;
					static const char *kwlist[] = {"index", NULL};
					if(!PyArg_ParseTupleAndKeywords(args, kws, "i:onRemove", (char**)kwlist, &idx))
						return (PyObject*) NULL;
					self->listModel->remove(idx);
				}
				Py_INCREF(Py_None);
				return Py_None;
			});
			registerEv("onClear", [selfWeakRef](PyObject* args, PyObject* kws) {
				ScopedRef selfWeakRefScope(selfWeakRef);
				QtListWidget* self = (QtListWidget*) selfWeakRefScope.get();
				if(self) {
					static const char *kwlist[] = {NULL};
					if(!PyArg_ParseTupleAndKeywords(args, kws, ":onClear", (char**)kwlist))
						return (PyObject*) NULL;
					self->listModel->clear();
				}
				Py_INCREF(Py_None);
				return Py_None;
			});
		}

finalInitialFill:
		if(lockEnterRes) {
			lockExitRes = PyObject_CallMethod(lock, (char*)"__exit__", (char*)"OOO", Py_None, Py_None, Py_None);
			if(!lockExitRes) {
				printf("Qt ListControl: list.lock.__exit__ failed\n");
				if(PyErr_Occurred())
					PyErr_Print();
			}
		}

		Py_XDECREF(lockEnterRes);
		Py_XDECREF(lockExitRes);
		Py_XDECREF(lock);
		Py_XDECREF(list);
	});

}

QtListWidget::~QtListWidget() {
	delete listWidget;
	listWidget = 0;
}

void QtListWidget::childIter(ChildIterCallback cb) {
	listModel->controlChildIter(cb);
}

void QtListWidget::updateContent() {
	// TODO...
}

void QtListWidget::resizeEvent(QResizeEvent* ev) {
	QtBaseWidget::resizeEvent(ev);
	listModel->updateLayout();
	listWidget->resize(size());
}

// TODO...
#if 0

@implementation ListControlView
{
	PyWeakReference* subjectListRef;
	std::vector<CocoaGuiObject*> guiObjectList;
	NSScrollView* scrollview;
	NSView* documentView;
	BOOL autoScrolldown;
	BOOL outstandingScrollviewUpdate;
	int selectionIndex; // for now, a single index. later maybe a range
	_NSFlippedView* dragCursor;
	PyWeakReference* dragHandlerRef;
	int dragIndex;
}

- (void)clearOwn
{
	// We expect to have the Python GIL.
	
	if(![NSThread isMainThread]) {
		Py_BEGIN_ALLOW_THREADS
		dispatch_sync(dispatch_get_main_queue(), ^{
			PyGILState_STATE gstate = PyGILState_Ensure();
			[self clearOwn];
			PyGILState_Release(gstate);
		});
		Py_END_ALLOW_THREADS
		return;
	}
	
	std::vector<CocoaGuiObject*> listCopy;
	std::swap(guiObjectList, listCopy);
	for(CocoaGuiObject* subCtr : listCopy) {
		NSView* child = subCtr->getNativeObj();
		if(child) [child removeFromSuperview];
		//		subCtr.obsolete = True
		Py_DECREF(subCtr);
	}
}

- (void)dealloc
{
	PyGILState_STATE gstate = PyGILState_Ensure();
	Py_CLEAR(subjectListRef);
	Py_CLEAR(dragHandlerRef);
	[self clearOwn];
	PyGILState_Release(gstate);
}

- (id)initWithControl:(CocoaGuiObject*)control
{
    self = [super initWithControl:control];
    if(!self) return nil;

	scrollview = [[NSScrollView alloc] initWithFrame:[self frame]];
	[scrollview setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
	[[scrollview contentView] setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
	documentView = [[_NSFlippedView alloc] initWithFrame:
					NSMakeRect(0, 0, [scrollview contentSize].width, [scrollview contentSize].height)];
	[scrollview setDocumentView:documentView];
	[documentView setAutoresizingMask:NSViewWidthSizable];
	[scrollview setHasVerticalScroller:YES];
	[scrollview setDrawsBackground:NO];
	[scrollview setBorderType:NSBezelBorder]; // NSGrooveBorder

	[self setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
	[self addSubview:scrollview];

	subjectListRef = NULL;
	outstandingScrollviewUpdate = FALSE;
	selectionIndex = -1;
	dragCursor = nil;
	dragHandlerRef = NULL;
	dragIndex = -1;
	
	// .... copied out: initial fill

    return self;
}

// Callback for subjectObject.
- (void)onInsert:(int)index withValue:(PyObject*) value
{
	PyGILState_STATE gstate = PyGILState_Ensure();

	if(![NSThread isMainThread]) {
		Py_BEGIN_ALLOW_THREADS
		dispatch_sync(dispatch_get_main_queue(), ^{ [self onInsert:index withValue:value]; });
		Py_END_ALLOW_THREADS
		return;
	}

	if(canHaveFocus && selectionIndex >= 0) {
		if(index <= selectionIndex) selectionIndex += 1;
	}
	
	if(index < 0 || index > guiObjectList.size()) {
		printf("Qt ListControl onInsert: invalid index %i, size=%zu\n", index, guiObjectList.size());
		index = (int) guiObjectList.size(); // maybe this recovering works...
		assert(index >= 0);
	}
	CocoaGuiObject* subCtr = [self buildControlForIndex:index andValue:value];
	if(subCtr) guiObjectList.insert(guiObjectList.begin() + index, subCtr);
	PyGILState_Release(gstate);
	
	[self scrollviewUpdate];
}

// Callback for subjectObject.
- (void)onRemove:(int)index
{
	PyGILState_STATE gstate = PyGILState_Ensure();

	if(![NSThread isMainThread]) {
		Py_BEGIN_ALLOW_THREADS
		dispatch_sync(dispatch_get_main_queue(), ^{ [self onRemove:index]; });
		Py_END_ALLOW_THREADS
		return;
	}
	
	if(canHaveFocus && selectionIndex >= 0) {
		if(index < selectionIndex) selectionIndex -= 1;
		else if(index == selectionIndex) [self deselect];
	}

	if(index >= 0 && index < guiObjectList.size()) {
		CocoaGuiObject* subCtr = guiObjectList[index];
		//	subCtr.obsolete = True
		NSView* child = subCtr->getNativeObj();
		if(child) [child removeFromSuperview];
		guiObjectList.erase(guiObjectList.begin() + index);
		Py_DECREF(subCtr);
	}
	PyGILState_Release(gstate);
	
	[self scrollviewUpdate];
}

// Callback for subjectObject.
- (void)onClear
{
	PyGILState_STATE gstate = PyGILState_Ensure();

	if(![NSThread isMainThread]) {
		Py_BEGIN_ALLOW_THREADS
		dispatch_sync(dispatch_get_main_queue(), ^{ [self onClear]; });
		Py_END_ALLOW_THREADS
		return;
	}

	selectionIndex = -1;
	[self clearOwn];
	PyGILState_Release(gstate);

	[self scrollviewUpdate];
}

- (void)removeInList:(int)index
{
	// don't run this in the main thread. it can lock.
	assert(![NSThread isMainThread]);

	PyGILState_STATE gstate = PyGILState_Ensure();
		
	PyObject* list = PyWeakref_GET_OBJECT(subjectListRef);
	PyObject* res = NULL;
	if(!list) goto final;
	
	res = PyObject_CallMethod(list, (char*)"remove", (char*)"(i)", index);
	if(!res || PyErr_Occurred()) {
		if(PyErr_Occurred())
			PyErr_Print();
	}
	
final:
	Py_XDECREF(list);
	Py_XDECREF(res);
	PyGILState_Release(gstate);
}

- (void)deselect
{
	if(selectionIndex >= 0 && selectionIndex < guiObjectList.size()) {
		CocoaGuiObject* subCtr = guiObjectList[selectionIndex];
		NSView* childView = subCtr->getNativeObj();
		if(childView && [childView respondsToSelector:@selector(setBackgroundColor:)])
			[childView performSelector:@selector(setBackgroundColor:) withObject:[NSColor textBackgroundColor]];
		selectionIndex = -1;
	}
}

- (void)select:(int)index
{
	[self deselect];

	PyGILState_STATE gstate = PyGILState_Ensure();
	
	if(index >= 0 && index < guiObjectList.size()) {
		selectionIndex = index;
		
		CocoaGuiObject* subCtr = guiObjectList[index];
		NSView* childView = subCtr->getNativeObj();
		if(childView && [childView respondsToSelector:@selector(setBackgroundColor:)])
			[childView performSelector:@selector(setBackgroundColor:) withObject:[NSColor selectedTextBackgroundColor]];
		
		subCtr->handleCurSelectedSong();
		
		dispatch_async(dispatch_get_main_queue(), ^{
			if(!childView || ![childView window]) return; // window closed or removed from window in the meantime
			NSRect objFrame = [childView frame];
			NSRect visibleFrame = [[scrollview contentView] documentVisibleRect];
			if(objFrame.origin.y < visibleFrame.origin.y)
				[[scrollview contentView] scrollToPoint:NSMakePoint(0, objFrame.origin.y)];
			else if(objFrame.origin.y + objFrame.size.height > visibleFrame.origin.y + visibleFrame.size.height)
				[[scrollview contentView] scrollToPoint:
				 NSMakePoint(0, objFrame.origin.y + objFrame.size.height - [scrollview contentSize].height)];
			[scrollview reflectScrolledClipView:[scrollview contentView]];
		});
	}
	
	PyGILState_Release(gstate);
}

- (void)doScrollviewUpdate
{
	if(!outstandingScrollviewUpdate) return;
	
	__block int x=0, y=0;
	int w = [scrollview contentSize].width;
	[self childIter:^(GuiObject* subCtr, bool& stop) {
		int h = subCtr->get_size(subCtr).y;
		subCtr->set_pos(subCtr, Vec(x,y));
		subCtr->set_size(subCtr, Vec(w,h));
		y += h;
	}];

	[documentView setFrameSize:NSMakeSize(w, y)];

	if(autoScrolldown) {
		[[scrollview verticalScroller] setFloatValue:1];
		[[scrollview contentView] scrollToPoint:
		NSMakePoint(0, [documentView frame].size.height - [scrollview contentSize].height)];
	}
	
	outstandingScrollviewUpdate = NO;
}

- (void)scrollviewUpdate
{
	if(outstandingScrollviewUpdate) return;
	outstandingScrollviewUpdate = YES;
	dispatch_async(dispatch_get_main_queue(), ^{ [self doScrollviewUpdate]; });
}

- (BOOL)becomeFirstResponder
{
	if(![super becomeFirstResponder]) return NO;
	if(selectionIndex < 0)
		[self select:0];
	return YES;
}

- (void)keyDown:(NSEvent *)ev
{
	if(!canHaveFocus) {
		[super keyDown:ev];
		return;
	}

	bool res = false;
	if([ev keyCode] == 125) { // down
		if(selectionIndex < 0)
			[self select:0];
		else if(selectionIndex < guiObjectList.size() - 1)
			[self select:selectionIndex+1];
		res = true;
	}
	else if([ev keyCode] == 126) { // up
		if(selectionIndex < 0)
			[self select:0];
		else if(selectionIndex > 0)
			[self select:selectionIndex-1];
		res = true;
	}
	else if([ev keyCode] == 0x33) { // delete
		if(selectionIndex >= 0 && selectionIndex < guiObjectList.size()) {
			int idx = selectionIndex;
			if(idx > 0)
				[self select:idx - 1];
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
				[self removeInList:idx];
			});
			res = true;
		}
	}
	else if([ev keyCode] == 0x75) { // forward delete
		if(selectionIndex >= 0 && selectionIndex < guiObjectList.size()) {
			int idx = selectionIndex;
			if(idx < guiObjectList.size() - 1)
				[self select:idx + 1];
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
				[self removeInList:idx];
			});
			res = true;
		}
	}

	if(!res)
		[super keyDown:ev];
}

- (void)mouseDown:(NSEvent *)theEvent
{
	if(!canHaveFocus) {
		[super mouseDown:theEvent];
		return;
	}
	
	bool res = false;
	[[self window] makeFirstResponder:self];
	
	NSPoint mouseLoc = [documentView convertPoint:[theEvent locationInWindow] toView:nil];
	PyGILState_STATE gstate = PyGILState_Ensure();
	for(int i = 0; i < (int)guiObjectList.size(); ++i) {
		if(NSPointInRect(mouseLoc, [guiObjectList[i]->getNativeObj() frame])) {
			[self select:i];
			res = true;
			break;
		}
	}
	PyGILState_Release(gstate);
	
	if(!res)
		[super mouseDown:theEvent];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
	return YES;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
	[dragCursor setDrawsBackground:YES];
	[documentView addSubview:dragCursor positioned:NSWindowAbove relativeTo:nil];
	NSPoint dragLoc = [documentView convertPoint:[sender draggingLocation] toView:nil];
	dragIndex = 0;
	
	CGFloat y = 0;
	PyGILState_STATE gstate = PyGILState_Ensure();
	for(int i = 0; i < (int)guiObjectList.size(); ++i) {
		NSRect frame = [guiObjectList[i]->getNativeObj() frame];
		if(dragLoc.y > frame.origin.y + frame.size.height / 2) {
			dragIndex = i + 1;
			y = frame.origin.y + frame.size.height;
		}
		else break;
	}
	PyGILState_Release(gstate);
	[dragCursor setFrameOrigin:NSMakePoint(0, y-1)];
	
	NSRect visibleFrame = [[scrollview contentView] documentVisibleRect];
	NSPoint mouseLoc = NSMakePoint(dragLoc.x - visibleFrame.origin.x, dragLoc.y - visibleFrame.origin.y);

	const CGFloat ScrollLimit = 30;
	const CGFloat Limit = 15;
	bool scroll = false;
	if(mouseLoc.y < Limit) {
		float scrollBy = Limit - mouseLoc.y;
		y = visibleFrame.origin.y - scrollBy;
		y = std::max(y, -ScrollLimit);
		scroll = true;
	}
	else if(mouseLoc.y > visibleFrame.size.height - Limit) {
		float scrollBy = mouseLoc.y - visibleFrame.size.height + Limit;
		y = visibleFrame.origin.y + scrollBy;
		y = std::min(y, [documentView frame].size.height - visibleFrame.size.height + ScrollLimit);
		scroll = true;
	}
	if(scroll) {
		[[scrollview contentView] scrollToPoint:NSMakePoint(0, y)];
		[scrollview reflectScrolledClipView:[scrollview contentView]];
	}
	
	return NSDragOperationGeneric;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
	if(dragCursor) {
		[dragCursor setDrawsBackground:NO];
		[dragCursor removeFromSuperview];
	}
	dragIndex = -1;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	if(dragCursor) {
		[dragCursor setDrawsBackground:NO];
		[dragCursor removeFromSuperview];
	}
	if(!dragHandlerRef) return NO;
	
	id dragSource = [sender draggingSource];
	NSArray* filenames = [[sender draggingPasteboard] propertyListForType:NSFilenamesPboardType];
	int index = dragIndex;
	
	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
		PyGILState_STATE gstate = PyGILState_Ensure();
		PyObject* dragHandler = PyWeakref_GET_OBJECT(dragHandlerRef);
		CocoaGuiObject* control = [self getControl];
		if(dragHandler && control) {
			Py_INCREF(dragHandler);
			Py_INCREF(control);
			CocoaGuiObject* parent = NULL;
			PyObject* res = NULL;
			PyObject* pyFilenames = NULL;

			parent = (CocoaGuiObject*) control->parent;
			if(!parent) {
				printf("Qt ListControl performDragOperation: control.parent is unset\n");
				goto finalCall;
			}
			Py_INCREF(parent);
			if(!PyType_IsSubtype(Py_TYPE(parent), &CocoaGuiObject_Type)) {
				printf("Qt ListControl performDragOperation: control.parent is wrong type\n");
				goto finalCall;
			}

			if(!control->subjectObject) {
				printf("Qt ListControl performDragOperation: control.subjectObject is unset\n");
				goto finalCall;
			}
			if(!parent->subjectObject) {
				printf("Qt ListControl performDragOperation: control.parent.subjectObject is unset\n");
				goto finalCall;
			}

			pyFilenames = PyTuple_New([filenames count]);
			if(!pyFilenames) {
				printf("Qt ListControl performDragOperation: failed to create pyFilenames\n");
				goto finalCall;
			}
			for(NSUInteger i = 0; i < [filenames count]; ++i) {
				NSString* objcStr = [filenames objectAtIndex:i];
				std::string cppStr([objcStr UTF8String]);
				PyObject* pyStr = PyUnicode_DecodeUTF8(&cppStr[0], cppStr.size(), NULL);
				if(!pyStr) {
					printf("Qt ListControl performDragOperation: failed to convert unicode filename\n");
					goto finalCall;
				}
				PyTuple_SET_ITEM(pyFilenames, i, pyStr);
			}

			res = PyObject_CallFunction(dragHandler, (char*)"(OOiO)", parent->subjectObject, control->subjectObject, index, pyFilenames);
			if(!res) {
				printf("Qt ListControl performDragOperation: dragHandler failed\n");
				goto finalCall;
			}

			{
				CocoaGuiObject* sourceControl = NULL;
				if([dragSource isKindOfClass:[GuiObjectView class]])
					sourceControl = [(GuiObjectView*)dragSource getControl];
				
				if(sourceControl) {
					GuiObject* parentControl = ((CocoaGuiObject*)sourceControl)->parent;
					Py_XINCREF(parentControl);
					if(parentControl && PyType_IsSubtype(Py_TYPE(parentControl), &CocoaGuiObject_Type)) {
						NSView* parentView = ((CocoaGuiObject*)parentControl)->getNativeObj();
						
						if([parentView isKindOfClass:[ListControlView class]]) {
							Py_INCREF(control);
							Py_INCREF(sourceControl);
							dispatch_async(dispatch_get_main_queue(), ^{
								PyGILState_STATE gstate = PyGILState_Ensure();
								[(ListControlView*)parentView onInternalDrag:control withObject:(CocoaGuiObject*)sourceControl withIndex:index withFiles:filenames];
								Py_DECREF(control);
								Py_DECREF(sourceControl);
								PyGILState_Release(gstate);
							});
						}
					}
					Py_XDECREF(parentControl);
				}
				
				Py_XDECREF(sourceControl);
			}
			
		finalCall:
			if(PyErr_Occurred()) PyErr_Print();
			Py_XDECREF(pyFilenames);
			Py_XDECREF(parent);
			Py_XDECREF(res);
			Py_DECREF(dragHandler);
			Py_DECREF(control);
		}
		Py_XDECREF(control);
		PyGILState_Release(gstate);
	});

	return YES;
}

- (void)onInternalDrag:(CocoaGuiObject*)destControl withObject:(CocoaGuiObject*)obj withIndex:(int)index withFiles:(NSArray*)filenames
{
	PyGILState_STATE gstate = PyGILState_Ensure();

	if(PyWeakref_GET_OBJECT(controlRef) == (PyObject*)destControl) { // internal drag to myself
		int oldIndex = selectionIndex;
		// check if the index is still correct
		if(oldIndex >= 0 && oldIndex < guiObjectList.size() && guiObjectList[oldIndex] == obj) {
			
			[self select:index];
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
				[self removeInList:oldIndex];
			});
		}
	}
	
	PyGILState_Release(gstate);
}

- (CocoaGuiObject*)buildControlForIndex:(int)index andValue:(PyObject*)value {
	if(![NSThread isMainThread]) {
		__block CocoaGuiObject* res = NULL;
		Py_BEGIN_ALLOW_THREADS
		dispatch_sync(dispatch_get_main_queue(), ^{
			PyGILState_STATE gstate = PyGILState_Ensure();
			res = [self buildControlForIndex:index andValue:value];
			PyGILState_Release(gstate);
		});
		Py_END_ALLOW_THREADS
		return res;
	}

	assert(value);
	CocoaGuiObject* subCtr = NULL;
	
	CocoaGuiObject* control = [self getControl];
	if(!control)
		// silently fail. we are probably just out-of-scope
		return NULL;
	
	subCtr = guiQt_createControlObject(value, control);

	PyObject* guiCocoaMod = getModule("guiCocoa"); // borrowed ref
	if(!guiCocoaMod) {
		printf("Qt ListControl buildControlForIndex: cannot get module gui\n");
		if(PyErr_Occurred()) PyErr_Print();
		Py_DECREF(subCtr);
		return NULL;
	}
	Py_INCREF(guiCocoaMod);

	{
		PyObject* res = PyObject_CallMethod(guiCocoaMod, (char*)"ListItem_AttrWrapper", (char*)"(iOO)", index, value, control);
		if(!res) {
			printf("Qt ListControl buildControlForIndex: cannot create ListItem_AttrWrapper\n");
			if(PyErr_Occurred()) PyErr_Print();
			Py_DECREF(subCtr);
			return NULL;
		}
		subCtr->attr = res; // overtake
	}

	subCtr->PresetSize.x = [scrollview contentSize].width;
	if(!guiObjectList.empty())
		subCtr->PresetSize.y = guiObjectList[0]->get_size(guiObjectList[0]).y;

	if(!_buildControlObject_pre(subCtr)) {
		Py_DECREF(subCtr);
		return NULL;
	}
	
	NSView* childView = subCtr->getNativeObj();
	if(!childView) {
		printf("Qt ListControl buildControlForIndex: subCtr.nativeGuiObject is nil\n");
		Py_DECREF(subCtr);
		return NULL;
	}
	[childView setAutoresizingMask:NSViewWidthSizable];
	// Move out of view so that there isn't any flickering while be lazily build this up.
	[childView setFrameOrigin:NSMakePoint(0, -[childView frame].size.height)];
	if(childView && [childView isKindOfClass:[_NSFlippedView class]])
		[(_NSFlippedView*)childView setDrawsBackground:YES];


	// do subCtr setup in background
	Py_INCREF(subCtr);
	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
		PyGILState_STATE gstate = PyGILState_Ensure();

		NSView* myView = nil;
		NSView* childView = nil;
		Vec sizeVec;
		CocoaGuiObject* control = [self getControl];
		if(!control) goto final; // silently fail. probably out-of-scope

		myView = control->getNativeObj();
		if(!myView) goto final; // silently fail. probably out-of-scope
		if(![myView window]) goto final; // window was closed

		childView = subCtr->getNativeObj();
		if(!childView) goto final; // silently fail. probably out-of-scope

		//	if getattr(subCtr, "obsolete", False): return # can happen in the meanwhile

		sizeVec = subCtr->setupChilds();
		
		{
			dispatch_async(dispatch_get_main_queue(), ^{
				int w = [scrollview contentSize].width;
				int h = sizeVec.y;
				[childView setFrameSize:NSMakeSize(w, h)];
			});

			Py_INCREF(subCtr);
			dispatch_async(dispatch_get_main_queue(), ^{
				PyGILState_STATE gstate = PyGILState_Ensure();
				_buildControlObject_post(subCtr);
				Py_DECREF(subCtr);
				PyGILState_Release(gstate);
			});

			Py_INCREF(subCtr);
			dispatch_async(dispatch_get_main_queue(), ^{
				PyGILState_STATE gstate = PyGILState_Ensure();
				subCtr->updateContent();
				Py_DECREF(subCtr);
				PyGILState_Release(gstate);
			});

			int w = subCtr->PresetSize.y;
			dispatch_async(dispatch_get_main_queue(), ^{
				// if getattr(subCtr, "obsolete", False): return # can happen in the meanwhile
				[[scrollview documentView] addSubview:childView];
				if(sizeVec.y != w)
					[self scrollviewUpdate];
			});
		}
		
	final:
		Py_DECREF(control);
		Py_DECREF(subCtr);
		PyGILState_Release(gstate);
	});

	Py_DECREF(control);
	Py_DECREF(guiCocoaMod);
	return subCtr;
}

@end

#endif
