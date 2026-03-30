/****************************************************************************
** Meta object code from reading C++ file 'ClipboardMonitor.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../../src/clipboard/ClipboardMonitor.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ClipboardMonitor.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ClipboardMonitor_t {
    QByteArrayData data[14];
    char stringdata0[169];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ClipboardMonitor_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ClipboardMonitor_t qt_meta_stringdata_ClipboardMonitor = {
    {
QT_MOC_LITERAL(0, 0, 16), // "ClipboardMonitor"
QT_MOC_LITERAL(1, 17, 20), // "localSnapshotChanged"
QT_MOC_LITERAL(2, 38, 0), // ""
QT_MOC_LITERAL(3, 39, 19), // "clipboard::Snapshot"
QT_MOC_LITERAL(4, 59, 8), // "snapshot"
QT_MOC_LITERAL(5, 68, 16), // "localTextChanged"
QT_MOC_LITERAL(6, 85, 4), // "text"
QT_MOC_LITERAL(7, 90, 8), // "textHash"
QT_MOC_LITERAL(8, 99, 17), // "localImageChanged"
QT_MOC_LITERAL(9, 117, 8), // "pngBytes"
QT_MOC_LITERAL(10, 126, 9), // "imageHash"
QT_MOC_LITERAL(11, 136, 17), // "localFilesChanged"
QT_MOC_LITERAL(12, 154, 5), // "paths"
QT_MOC_LITERAL(13, 160, 8) // "listHash"

    },
    "ClipboardMonitor\0localSnapshotChanged\0"
    "\0clipboard::Snapshot\0snapshot\0"
    "localTextChanged\0text\0textHash\0"
    "localImageChanged\0pngBytes\0imageHash\0"
    "localFilesChanged\0paths\0listHash"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ClipboardMonitor[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       4,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   34,    2, 0x06 /* Public */,
       5,    2,   37,    2, 0x06 /* Public */,
       8,    2,   42,    2, 0x06 /* Public */,
      11,    2,   47,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::UInt,    6,    7,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::UInt,    9,   10,
    QMetaType::Void, QMetaType::QStringList, QMetaType::UInt,   12,   13,

       0        // eod
};

void ClipboardMonitor::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ClipboardMonitor *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->localSnapshotChanged((*reinterpret_cast< const clipboard::Snapshot(*)>(_a[1]))); break;
        case 1: _t->localTextChanged((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2]))); break;
        case 2: _t->localImageChanged((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2]))); break;
        case 3: _t->localFilesChanged((*reinterpret_cast< const QStringList(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< clipboard::Snapshot >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ClipboardMonitor::*)(const clipboard::Snapshot & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClipboardMonitor::localSnapshotChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ClipboardMonitor::*)(const QString & , quint32 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClipboardMonitor::localTextChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ClipboardMonitor::*)(const QByteArray & , quint32 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClipboardMonitor::localImageChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (ClipboardMonitor::*)(const QStringList & , quint32 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClipboardMonitor::localFilesChanged)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ClipboardMonitor::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ClipboardMonitor.data,
    qt_meta_data_ClipboardMonitor,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ClipboardMonitor::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ClipboardMonitor::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ClipboardMonitor.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ClipboardMonitor::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void ClipboardMonitor::localSnapshotChanged(const clipboard::Snapshot & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ClipboardMonitor::localTextChanged(const QString & _t1, quint32 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void ClipboardMonitor::localImageChanged(const QByteArray & _t1, quint32 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void ClipboardMonitor::localFilesChanged(const QStringList & _t1, quint32 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
