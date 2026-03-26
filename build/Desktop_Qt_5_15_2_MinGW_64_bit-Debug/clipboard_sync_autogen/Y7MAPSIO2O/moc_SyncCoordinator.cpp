/****************************************************************************
** Meta object code from reading C++ file 'SyncCoordinator.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../../src/sync/SyncCoordinator.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'SyncCoordinator.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_SyncCoordinator_t {
    QByteArrayData data[11];
    char stringdata0[146];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_SyncCoordinator_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_SyncCoordinator_t qt_meta_stringdata_SyncCoordinator = {
    {
QT_MOC_LITERAL(0, 0, 15), // "SyncCoordinator"
QT_MOC_LITERAL(1, 16, 18), // "localTextForwarded"
QT_MOC_LITERAL(2, 35, 0), // ""
QT_MOC_LITERAL(3, 36, 4), // "text"
QT_MOC_LITERAL(4, 41, 19), // "localFilesForwarded"
QT_MOC_LITERAL(5, 61, 5), // "paths"
QT_MOC_LITERAL(6, 67, 18), // "remoteTextReceived"
QT_MOC_LITERAL(7, 86, 23), // "remoteFileOfferReceived"
QT_MOC_LITERAL(8, 110, 9), // "fileNames"
QT_MOC_LITERAL(9, 120, 18), // "fileTransferStatus"
QT_MOC_LITERAL(10, 139, 6) // "status"

    },
    "SyncCoordinator\0localTextForwarded\0\0"
    "text\0localFilesForwarded\0paths\0"
    "remoteTextReceived\0remoteFileOfferReceived\0"
    "fileNames\0fileTransferStatus\0status"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_SyncCoordinator[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,
       4,    1,   42,    2, 0x06 /* Public */,
       6,    1,   45,    2, 0x06 /* Public */,
       7,    1,   48,    2, 0x06 /* Public */,
       9,    1,   51,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QStringList,    5,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QStringList,    8,
    QMetaType::Void, QMetaType::QString,   10,

       0        // eod
};

void SyncCoordinator::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<SyncCoordinator *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->localTextForwarded((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->localFilesForwarded((*reinterpret_cast< const QStringList(*)>(_a[1]))); break;
        case 2: _t->remoteTextReceived((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->remoteFileOfferReceived((*reinterpret_cast< const QStringList(*)>(_a[1]))); break;
        case 4: _t->fileTransferStatus((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (SyncCoordinator::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SyncCoordinator::localTextForwarded)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (SyncCoordinator::*)(const QStringList & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SyncCoordinator::localFilesForwarded)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (SyncCoordinator::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SyncCoordinator::remoteTextReceived)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (SyncCoordinator::*)(const QStringList & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SyncCoordinator::remoteFileOfferReceived)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (SyncCoordinator::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&SyncCoordinator::fileTransferStatus)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject SyncCoordinator::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_SyncCoordinator.data,
    qt_meta_data_SyncCoordinator,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *SyncCoordinator::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SyncCoordinator::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_SyncCoordinator.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SyncCoordinator::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void SyncCoordinator::localTextForwarded(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void SyncCoordinator::localFilesForwarded(const QStringList & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void SyncCoordinator::remoteTextReceived(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void SyncCoordinator::remoteFileOfferReceived(const QStringList & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void SyncCoordinator::fileTransferStatus(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
