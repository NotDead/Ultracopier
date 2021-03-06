#include "ReadThread.h"

ReadThread::ReadThread()
{
    start();
    moveToThread(this);
    stopIt=false;
    putInPause=false;
    blockSize=ULTRACOPIER_PLUGIN_DEFAULT_BLOCK_SIZE*1024;
    setObjectName(QStringLiteral("read"));
    #ifdef ULTRACOPIER_PLUGIN_DEBUG
    stat=Idle;
    #endif
    isInReadLoop=false;
    tryStartRead=false;
    lastGoodPosition=0;
    isOpen.release();
}

ReadThread::~ReadThread()
{
    stopIt=true;
    //disconnect(this);//-> do into ~TransferThread()
    #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
    waitNewClockForSpeed.release();
    #endif
    pauseMutex.release();
    #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
    waitNewClockForSpeed.release();
    #endif
    pauseMutex.release();
    //if(isOpen.available()<=0)
        emit internalStartClose();
    isOpen.acquire();
    exit();
    wait();
}

void ReadThread::run()
{
    connect(this,&ReadThread::internalStartOpen,		this,&ReadThread::internalOpenSlot,     Qt::QueuedConnection);
    connect(this,&ReadThread::internalStartReopen,		this,&ReadThread::internalReopen,       Qt::QueuedConnection);
    connect(this,&ReadThread::internalStartRead,		this,&ReadThread::internalRead,         Qt::QueuedConnection);
    connect(this,&ReadThread::internalStartClose,		this,&ReadThread::internalCloseSlot,	Qt::QueuedConnection);
    connect(this,&ReadThread::checkIfIsWait,            this,&ReadThread::isInWait,             Qt::QueuedConnection);
    connect(this,&ReadThread::internalStartChecksum,	this,&ReadThread::checkSum,             Qt::QueuedConnection);
    exec();
}

void ReadThread::open(const QFileInfo &file, const Ultracopier::CopyMode &mode)
{
    if(!isRunning())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] the thread not running to open destination: ")+file.absoluteFilePath());
        errorString_internal=tr("Internal error, please report it!");
        emit error();
    }
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] open source: ")+file.absoluteFilePath());
    if(this->file.isOpen())
    {
        if(file.absoluteFilePath()==this->file.fileName())
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] Try reopen already opened same file: ")+file.absoluteFilePath());
        else
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Critical,QStringLiteral("[")+QString::number(id)+QStringLiteral("] previous file is already open: ")+file.absoluteFilePath());
        emit internalStartClose();
        isOpen.acquire();
        isOpen.release();
    }
    if(isInReadLoop)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Critical,QStringLiteral("[")+QString::number(id)+QStringLiteral("] previous file is already readding: ")+file.absoluteFilePath());
        return;
    }
    if(tryStartRead)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Critical,QStringLiteral("[")+QString::number(id)+QStringLiteral("] previous file is already try read: ")+file.absoluteFilePath());
        return;
    }
    stopIt=false;
    fakeMode=false;
    lastGoodPosition=0;
    this->file.setFileName(file.absoluteFilePath());
    this->mode=mode;
    emit internalStartOpen();
}

QString ReadThread::errorString() const
{
    return errorString_internal;
}

void ReadThread::stop()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stop()"));
    stopIt=true;
    pauseMutex.release();
    pauseMutex.release();
    #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
    waitNewClockForSpeed.release();
    #endif
    if(isOpen.available()<=0)
        emit internalStartClose();
}

void ReadThread::pause()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] try put read thread in pause"));
    if(stopIt)
        return;
    pauseMutex.tryAcquire(pauseMutex.available());
    putInPause=true;
}

void ReadThread::resume()
{
    if(putInPause)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
        putInPause=false;
        stopIt=false;
    }
    else
        return;
    if(!file.isOpen())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] file is not open"));
        return;
    }
    pauseMutex.release();
}

bool ReadThread::seek(const qint64 &position)
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start with: ")+QString::number(position));
    if(position>file.size())
        return false;
    return file.seek(position);
}

qint64 ReadThread::size() const
{
    return file.size();
}

void ReadThread::postOperation()
{
    emit internalStartClose();
}

void ReadThread::checkSum()
{
    QByteArray blockArray;
    QCryptographicHash hash(QCryptographicHash::Sha1);
    isInReadLoop=true;
    lastGoodPosition=0;
    #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
    numberOfBlockCopied=0;
    #endif
    seek(0);
    int sizeReaden=0;
    do
    {
        //read one block
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Read;
        #endif
        if(putInPause)
        {
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Information,QStringLiteral("[")+QString::number(id)+QStringLiteral("] read put in pause"));
            if(stopIt)
                return;
            pauseMutex.acquire();
            if(stopIt)
                return;
        }
        blockArray=file.read(blockSize);
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Idle;
        #endif

        //can be smaller than min block size to do correct speed limitation
        if(blockArray.size()>ULTRACOPIER_PLUGIN_MAX_BLOCK_SIZE*1024)
        {
            errorString_internal=tr("Internal error reading the source file:block size out of range");
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Internal error reading the source file:block size out of range"));
            emit error();
            isInReadLoop=false;
            return;
        }
        if(file.error()!=QFile::NoError)
        {
            errorString_internal=tr("Unable to read the source file: ")+file.errorString()+QStringLiteral(" (")+QString::number(file.error())+QStringLiteral(")");
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("file.error()!=QFile::NoError: %1, error: %2").arg(QString::number(file.error())).arg(errorString_internal));
            emit error();
            isInReadLoop=false;
            return;
        }
        sizeReaden=blockArray.size();
        if(sizeReaden>0)
        {
            #ifdef ULTRACOPIER_PLUGIN_DEBUG
            stat=Checksum;
            #endif
            hash.addData(blockArray);
            #ifdef ULTRACOPIER_PLUGIN_DEBUG
            stat=Idle;
            #endif

            if(stopIt)
                break;

            lastGoodPosition+=blockArray.size();

            #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
            //wait for limitation speed if stop not query
            if(multiForBigSpeed>0)
            {
                numberOfBlockCopied++;
                if(numberOfBlockCopied>=multiForBigSpeed)
                {
                    numberOfBlockCopied=0;
                    waitNewClockForSpeed.acquire();
                    if(stopIt)
                        break;
                }
            }
            #endif
        }
    }
    while(sizeReaden>0 && !stopIt);
    if(lastGoodPosition>file.size())
    {
        errorString_internal=tr("File truncated during the read, possible data change");
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Source truncated during the read: %1 (%2)").arg(file.errorString()).arg(QString::number(file.error())));
        emit error();
        isInReadLoop=false;
        return;
    }
    isInReadLoop=false;
    if(stopIt)
    {
        stopIt=false;
        return;
    }
    emit checksumFinish(hash.result());
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stop the read"));
}

bool ReadThread::internalOpenSlot()
{
    return internalOpen();
}

bool ReadThread::internalOpen(bool resetLastGoodPosition)
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] internalOpen source: ")+file.fileName()+QStringLiteral(", open in write because move: ")+QString::number(mode==Ultracopier::Move));
    if(stopIt)
    {
        emit closed();
        return false;
    }
    putInPause=false;
    #ifdef ULTRACOPIER_PLUGIN_DEBUG
    stat=InodeOperation;
    #endif
    if(file.isOpen())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] this file is already open: ")+file.fileName());
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Idle;
        #endif
        emit closed();
        return false;
    }
    QIODevice::OpenMode openMode=QIODevice::ReadOnly;
    /*can have permision to remove but not write
     * if(mode==Ultracopier::Move)
        openMode=QIODevice::ReadWrite;*/
    seekToZero=false;
    if(file.open(openMode))
    {
        if(stopIt)
        {
            file.close();
            emit closed();
            return false;
        }
        pauseMutex.tryAcquire(pauseMutex.available());
        if(stopIt)
        {
            file.close();
            emit closed();
            return false;
        }
        size_at_open=file.size();
        mtime_at_open=QFileInfo(file).lastModified();
        putInPause=false;
        if(resetLastGoodPosition)
            lastGoodPosition=0;
        if(!seek(lastGoodPosition))
        {
            file.close();
            errorString_internal=file.errorString();
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Unable to seek after open: %1, error: %2").arg(file.fileName()).arg(errorString_internal));
            emit error();
            #ifdef ULTRACOPIER_PLUGIN_DEBUG
            stat=Idle;
            #endif
            return false;
        }
        isOpen.acquire();
        emit opened();
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Idle;
        #endif
        return true;
    }
    else
    {
        errorString_internal=file.errorString();
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Unable to open: %1, error: %2").arg(file.fileName()).arg(errorString_internal));
        emit error();
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Idle;
        #endif
        return false;
    }
}

void ReadThread::internalRead()
{
    isInReadLoop=true;
    tryStartRead=false;
    if(stopIt)
    {
        if(seekToZero && file.isOpen())
        {
            stopIt=false;
            lastGoodPosition=0;
            file.seek(0);
        }
        else
        {
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopIt == true, then quit"));
            isInReadLoop=false;
            internalClose();
            return;
        }
    }
    #ifdef ULTRACOPIER_PLUGIN_DEBUG
    stat=InodeOperation;
    #endif
    int sizeReaden=0;
    if(!file.isOpen())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] is not open!"));
        isInReadLoop=false;
        return;
    }
    QByteArray blockArray;
    #ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
    numberOfBlockCopied=0;
    #endif
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start the copy"));
    emit readIsStarted();
    #ifdef ULTRACOPIER_PLUGIN_DEBUG
    stat=Idle;
    #endif
    if(stopIt)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopIt == true, then quit"));
        isInReadLoop=false;
        internalClose();
        return;
    }
    do
    {
        //read one block
        if(putInPause)
        {
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Information,QStringLiteral("[")+QString::number(id)+QStringLiteral("] read put in pause"));
            if(stopIt)
            {
                ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopIt == true, then quit"));
                isInReadLoop=false;
                internalClose();
                return;
            }
            pauseMutex.acquire();
            if(stopIt)
            {
                ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopIt == true, then quit"));
                isInReadLoop=false;
                internalClose();
                return;
            }
        }
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Read;
        #endif
        blockArray=file.read(blockSize);
        #ifdef ULTRACOPIER_PLUGIN_DEBUG
        stat=Idle;
        #endif

        if(file.error()!=QFile::NoError)
        {
            errorString_internal=tr("Unable to read the source file: ")+file.errorString()+QStringLiteral(" (")+QString::number(file.error())+QStringLiteral(")");
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("file.error()!=QFile::NoError: %1, error: %2").arg(QString::number(file.error())).arg(errorString_internal));
            isInReadLoop=false;
            emit error();
            return;
        }
        sizeReaden=blockArray.size();
        if(sizeReaden>0)
        {
            #ifdef ULTRACOPIER_PLUGIN_DEBUG
            stat=WaitWritePipe;
            #endif
            if(!writeThread->write(blockArray))//speed limitation here
            {
                if(!stopIt)
                {
                    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopped because the write is stopped: ")+QString::number(lastGoodPosition));
                    stopIt=true;
                }
            }

            #ifdef ULTRACOPIER_PLUGIN_DEBUG
            stat=Idle;
            #endif

            if(stopIt)
            {
                ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stopIt == true, then quit"));
                isInReadLoop=false;
                internalClose();//need re-open the destination and then the source
                return;
            }
            lastGoodPosition+=blockArray.size();
        }
        /*
        if(lastGoodPosition>16*1024)
        {
            ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Test error in reading: %1 (%2)").arg(file.errorString()).arg(file.error()));
            errorString_internal=QStringLiteral("Test error in reading: %1 (%2)").arg(file.errorString()).arg(file.error());
            isInReadLoop=false;
            emit error();
            return;
        }
        */
    }
    while(sizeReaden>0 && !stopIt);
    if(lastGoodPosition>file.size())
    {
        errorString_internal=tr("File truncated during the read, possible data change");
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] ")+QStringLiteral("Source truncated during the read: %1 (%2)").arg(file.errorString()).arg(QString::number(file.error())));
        isInReadLoop=false;
        emit error();
        return;
    }
    isInReadLoop=false;
    if(stopIt)
    {
        stopIt=false;
        return;
    }
    emit readIsStopped();//will product by signal connection writeThread->endIsDetected();
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] stop the read"));
}

void ReadThread::startRead()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    if(tryStartRead)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] already in try start"));
        return;
    }
    if(isInReadLoop)
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] double event dropped"));
    else
    {
        tryStartRead=true;
        emit internalStartRead();
    }
}

void ReadThread::internalCloseSlot()
{
    internalClose();
}

void ReadThread::internalClose(bool callByTheDestructor)
{
    /// \note never send signal here, because it's called by the destructor
    //ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    bool closeTheFile=false;
    if(!fakeMode)
    {
        if(file.isOpen())
        {
            closeTheFile=true;
            file.close();
            isInReadLoop=false;
        }
    }
    if(!callByTheDestructor)
        emit closed();

    /// \note always the last of this function
    if(closeTheFile)
        isOpen.release();
}

/** \brief set block size
\param block the new block size in B
\return Return true if succes */
bool ReadThread::setBlockSize(const int blockSize)
{
    //can be smaller than min block size to do correct speed limitation
    if(blockSize>1 && blockSize<ULTRACOPIER_PLUGIN_MAX_BLOCK_SIZE*1024)
    {
        this->blockSize=blockSize;
        //set the new max speed because the timer have changed
        return true;
    }
    else
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("block size out of range: ")+QString::number(blockSize));
        return false;
    }
}

#ifdef ULTRACOPIER_PLUGIN_SPEED_SUPPORT
/*! \brief Set the max speed
\param tempMaxSpeed Set the max speed in KB/s, 0 for no limit */
void ReadThread::setMultiForBigSpeed(const int &multiForBigSpeed)
{
    this->multiForBigSpeed=multiForBigSpeed;
    waitNewClockForSpeed.release();
}

/// \brief For give timer every X ms
void ReadThread::timeOfTheBlockCopyFinished()
{
    /* this is the old way to limit the speed, it product blocking
     *if(waitNewClockForSpeed.available()<ULTRACOPIER_PLUGIN_NUMSEMSPEEDMANAGEMENT)
        waitNewClockForSpeed.release();*/

    //try this new way:
    /* only if speed limited, else will accumulate waitNewClockForSpeed
     * Disabled because: Stop call of this method when no speed limit
    if(this->maxSpeed>0)*/
    if(waitNewClockForSpeed.available()<=1)
        waitNewClockForSpeed.release();
}
#endif

/// \brief do the fake open
void ReadThread::fakeOpen()
{
    fakeMode=true;
    emit opened();
}

/// \brief do the fake writeIsStarted
void ReadThread::fakeReadIsStarted()
{
    emit readIsStarted();
}

/// \brief do the fake writeIsStopped
void ReadThread::fakeReadIsStopped()
{
    emit readIsStopped();
}

/// do the checksum
void ReadThread::startCheckSum()
{
    emit internalStartChecksum();
}

qint64 ReadThread::getLastGoodPosition() const
{
    /*if(lastGoodPosition>file.size())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Critical,QStringLiteral("[")+QString::number(id)+QStringLiteral("] Bug, the lastGoodPosition is greater than the file size!"));
        return file.size();
    }
    else*/
    return lastGoodPosition;
}

//reopen after an error
void ReadThread::reopen()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    if(isInReadLoop)
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] try reopen where read is not finish"));
        return;
    }
    stopIt=true;
    emit internalStartReopen();
}

bool ReadThread::internalReopen()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    stopIt=false;
    if(file.isOpen())
    {
        file.close();
        isOpen.release();
    }
    if(size_at_open!=file.size() && mtime_at_open!=QFileInfo(file).lastModified())
    {
        ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Warning,QStringLiteral("[")+QString::number(id)+QStringLiteral("] source file have changed since the last open, restart all"));
        //fix this function like the close function
        if(internalOpen(true))
        {
            emit resumeAfterErrorByRestartAll();
            return true;
        }
        else
            return false;
    }
    else
    {
        //fix this function like the close function
        if(internalOpen(false))
        {
            emit resumeAfterErrorByRestartAtTheLastPosition();
            return true;
        }
        else
            return false;
    }
}

//set the write thread
void ReadThread::setWriteThread(WriteThread * writeThread)
{
    this->writeThread=writeThread;
}

#ifdef ULTRACOPIER_PLUGIN_DEBUG
//to set the id
void ReadThread::setId(int id)
{
    this->id=id;
}
#endif

void ReadThread::seekToZeroAndWait()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    stopIt=true;
    seekToZero=true;
    emit checkIfIsWait();
}

void ReadThread::isInWait()
{
    ULTRACOPIER_DEBUGCONSOLE(Ultracopier::DebugLevel_Notice,QStringLiteral("[")+QString::number(id)+QStringLiteral("] start"));
    if(seekToZero)
    {
        stopIt=false;
        seekToZero=false;
        if(file.isOpen())
        {
            lastGoodPosition=0;
            seek(0);
        }
        else
            internalOpen(true);
        emit isSeekToZeroAndWait();
    }
}

bool ReadThread::isReading() const
{
    return isInReadLoop;
}

