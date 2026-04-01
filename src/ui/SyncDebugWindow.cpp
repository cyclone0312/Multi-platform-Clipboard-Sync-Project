#include "ui/SyncDebugWindow.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    // 从 Qt 的拖放负载里提取本地文件路径。这里只接收真实本地文件，
    // 因为当前传输层只知道怎样读取并发送本地文件内容。
    QStringList extractLocalPaths(const QMimeData *mimeData)
    {
        QStringList paths;
        if (!mimeData || !mimeData->hasUrls())
        {
            return paths;
        }

        const QList<QUrl> urls = mimeData->urls();
        paths.reserve(urls.size());
        for (const QUrl &url : urls)
        {
            if (!url.isLocalFile())
            {
                continue;
            }

            const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
            if (!path.isEmpty())
            {
                paths.push_back(path);
            }
        }

        return paths;
    }

    class FileDropLabel final : public QLabel
    {
        Q_OBJECT

    public:
        explicit FileDropLabel(QWidget *parent = nullptr)
            : QLabel(parent)
        {
            // 这个控件就是新增“拖入窗口传输”功能的明确入口。
            setAcceptDrops(true);
            setAlignment(Qt::AlignCenter);
            setWordWrap(true);
            setMinimumHeight(84);
            setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
            setText(QStringLiteral("将文件拖到这里，直接发送到对端窗口"));
            setStyleSheet(QStringLiteral("QLabel { border: 1px dashed #6b7280; padding: 12px; background: #f8fafc; }"));
        }

    signals:
        void filesDropped(const QStringList &paths);

    protected:
        void dragEnterEvent(QDragEnterEvent *event) override
        {
            const QStringList paths = extractLocalPaths(event ? event->mimeData() : nullptr);
            if (paths.isEmpty())
            {
                event->ignore();
                return;
            }

            event->acceptProposedAction();
        }

        void dropEvent(QDropEvent *event) override
        {
            const QStringList paths = extractLocalPaths(event ? event->mimeData() : nullptr);
            if (paths.isEmpty())
            {
                event->ignore();
                return;
            }

            emit filesDropped(paths);
            event->acceptProposedAction();
        }
    };

    class ReadyFileListWidget final : public QListWidget
    {
    public:
        explicit ReadyFileListWidget(QWidget *parent = nullptr)
            : QListWidget(parent)
        {
            setSelectionMode(QAbstractItemView::ExtendedSelection);
            setDragEnabled(true);
            setAlternatingRowColors(true);
            setDefaultDropAction(Qt::CopyAction);
        }

    protected:
        void startDrag(Qt::DropActions supportedActions) override
        {
            Q_UNUSED(supportedActions)

            // 这里只对外声明已经真实存在于本地磁盘的文件，
            // 这样拖到资源管理器等目标时不需要再等待网络下载。
            QList<QUrl> urls;
            const QList<QListWidgetItem *> items = selectedItems();
            urls.reserve(items.size());
            for (QListWidgetItem *item : items)
            {
                if (!item)
                {
                    continue;
                }

                const QString path = item->data(Qt::UserRole).toString();
                if (!QFileInfo::exists(path))
                {
                    continue;
                }

                urls.push_back(QUrl::fromLocalFile(path));
            }

            if (urls.isEmpty())
            {
                return;
            }

            auto *mimeData = new QMimeData();
            mimeData->setUrls(urls);

            auto *drag = new QDrag(this);
            drag->setMimeData(mimeData);
            drag->exec(Qt::CopyAction);
        }
    };
}

SyncDebugWindow::SyncDebugWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Clipboard Sync Monitor"));
    resize(860, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *tip = new QLabel(QStringLiteral("上方显示本地复制并外发的内容，下方显示收到的远端内容。"), this);
    root->addWidget(tip);

    auto *manualGroup = new QGroupBox(QStringLiteral("手动添加（写本地并发送对端）"), this);
    auto *manualLayout = new QVBoxLayout(manualGroup);
    m_manualInput = new QPlainTextEdit(manualGroup);
    m_manualInput->setPlaceholderText(QStringLiteral("输入要写入剪贴板并发送给对端的文本"));
    m_manualInput->setMaximumHeight(90);
    m_manualSendButton = new QPushButton(QStringLiteral("写入本地并发送"), manualGroup);
    m_requestRemoteFilesButton = new QPushButton(QStringLiteral("模拟粘贴触发远端文件拉取"), manualGroup);
    manualLayout->addWidget(m_manualInput);
    manualLayout->addWidget(m_manualSendButton);
    manualLayout->addWidget(m_requestRemoteFilesButton);
    auto *dropLabel = new FileDropLabel(manualGroup);
    m_dropZone = dropLabel;
    manualLayout->addWidget(dropLabel);
    QObject::connect(m_manualSendButton, &QPushButton::clicked, this, &SyncDebugWindow::onManualSendClicked);
    QObject::connect(m_requestRemoteFilesButton, &QPushButton::clicked, this, &SyncDebugWindow::onRequestRemoteFilesClicked);
    QObject::connect(dropLabel, &FileDropLabel::filesDropped, this, &SyncDebugWindow::localFilesDropped);
    root->addWidget(manualGroup);

    auto *localGroup = new QGroupBox(QStringLiteral("本地复制（发送侧）"), this);
    auto *localLayout = new QVBoxLayout(localGroup);
    m_localView = new QPlainTextEdit(localGroup);
    m_localView->setReadOnly(true);
    m_localView->setMaximumBlockCount(m_maxBlocks);
    localLayout->addWidget(m_localView);

    auto *readyGroup = new QGroupBox(QStringLiteral("已下载完成（可拖出）"), this);
    auto *readyLayout = new QVBoxLayout(readyGroup);
    m_readyFileList = new ReadyFileListWidget(readyGroup);
    m_readyFileList->setToolTip(QStringLiteral("将已完成下载的文件从这里拖到桌面、资源管理器或其他应用。"));
    readyLayout->addWidget(m_readyFileList);

    auto *remoteGroup = new QGroupBox(QStringLiteral("远端接收（接收侧）"), this);
    auto *remoteLayout = new QVBoxLayout(remoteGroup);
    m_remoteView = new QPlainTextEdit(remoteGroup);
    m_remoteView->setReadOnly(true);
    m_remoteView->setMaximumBlockCount(m_maxBlocks);
    remoteLayout->addWidget(m_remoteView);

    auto *contentLayout = new QHBoxLayout();
    auto *rightColumn = new QVBoxLayout();
    rightColumn->addWidget(readyGroup, 1);
    rightColumn->addWidget(remoteGroup, 1);
    contentLayout->addWidget(localGroup, 1);
    contentLayout->addLayout(rightColumn, 1);
    root->addLayout(contentLayout, 1);
}

void SyncDebugWindow::appendLocalText(const QString &text)
{
    appendEntry(m_localView, text);
}

void SyncDebugWindow::appendRemoteText(const QString &text)
{
    appendEntry(m_remoteView, text);
}

void SyncDebugWindow::appendFileTransferStatus(const QString &status)
{
    appendEntry(m_remoteView, QStringLiteral("[File] %1").arg(status));
}

void SyncDebugWindow::appendDownloadedFiles(const QStringList &paths)
{
    if (!m_readyFileList || paths.isEmpty())
    {
        return;
    }

    for (const QString &path : paths)
    {
        // 把绝对路径放进 UserRole，后续 startDrag() 就能直接重建标准的
        // text/uri-list 负载，而不用再从显示文本里反解析路径。
        const QFileInfo info(path);
        auto *item = new QListWidgetItem(info.fileName().isEmpty() ? path : info.fileName(), m_readyFileList);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
}

void SyncDebugWindow::onManualSendClicked()
{
    if (!m_manualInput)
    {
        return;
    }

    const QString text = m_manualInput->toPlainText();
    if (text.trimmed().isEmpty())
    {
        return;
    }

    emit manualInjectRequested(text);
}

void SyncDebugWindow::onRequestRemoteFilesClicked()
{
    emit requestRemoteFilesTriggered();
}

void SyncDebugWindow::appendEntry(QPlainTextEdit *target, const QString &text)
{
    if (!target)
    {
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    if (!target->document()->isEmpty())
    {
        target->appendPlainText(QString());
    }
    target->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, text));
}

#include "SyncDebugWindow.moc"
