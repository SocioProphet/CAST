/*******************************************************************************
 |    bbio.cc
 |
 |  � Copyright IBM Corporation 2015,2016. All Rights Reserved
 |
 |    This program is licensed under the terms of the Eclipse Public License
 |    v1.0 as published by the Eclipse Foundation and available at
 |    http://www.eclipse.org/legal/epl-v10.html
 |
 |    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
 |    restricted by GSA ADP Schedule Contract with IBM Corp.
 *******************************************************************************/

#include <map>
#include <queue>

#include "bbio.h"
#include "bbinternal.h"
#include "bbserver_flightlog.h"
#include "Extent.h"
#include "tracksyscall.h"

extern size_t             transferBufferSize;
thread_local char*        threadTransferBuffer;

static pthread_mutex_t    freeTransferBuffers_mutex = PTHREAD_MUTEX_INITIALIZER;
static queue<void*>       freeTransferBuffers;
static sem_t              numFreeBuffers;

static uint64_t l_Total_SSD_Reads = 0;
static uint64_t l_Total_SSD_Writes = 0;

static int l_Governors_Inited = 0;
static int l_SSD_Read_Governor_Active = 0;
static int l_SSD_Write_Governor_Active = 0;
static sem_t l_SSD_Read_Governor;
static sem_t l_SSD_Write_Governor;


int setupTransferBuffer(int fileindex)
{
    if(threadTransferBuffer == NULL)
    {
        unsigned int backlog;
        sem_wait(&numFreeBuffers);
        pthread_mutex_lock(&freeTransferBuffers_mutex);
        threadTransferBuffer = (char*)freeTransferBuffers.front();
        freeTransferBuffers.pop();
        backlog = freeTransferBuffers.size();
        pthread_mutex_unlock(&freeTransferBuffers_mutex);

        FL_Write(FLXfer, FLXBufPop, "Using buffer %p  available buffers=%ld",(uint64_t)threadTransferBuffer,backlog,0,0);
    }
    return 0;
}
int returnTransferBuffer(void* buffer)
{
    unsigned int backlog;
    pthread_mutex_lock(&freeTransferBuffers_mutex);
    freeTransferBuffers.push(buffer);
    backlog = freeTransferBuffers.size();
    pthread_mutex_unlock(&freeTransferBuffers_mutex);
    FL_Write(FLXfer, FLXBufPush, "Using buffer %p  available buffers=%ld",(uint64_t)buffer,backlog,0,0);
    sem_post(&numFreeBuffers);
    return 0;
}



string getDeviceBySerial(string serial);

static map<string, int>   Serial2ssdReadFd;
static map<string, int>   Serial2ssdWriteFd;
static pthread_mutex_t    ssdReadFdMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t    ssdWriteFdMutex = PTHREAD_MUTEX_INITIALIZER;

const unsigned int PAGESIZE=getpagesize();
const unsigned int PAGESIZEOFFSETMASK=PAGESIZE-1;
const unsigned int FULLPAGESMASK=  ~PAGESIZEOFFSETMASK;

static unsigned int ssdwritedirect = 0;

unsigned int pageRounder(int length){
   unsigned int rounded=length;
   if (PAGESIZEOFFSETMASK & rounded) {
      rounded = (rounded & FULLPAGESMASK) + PAGESIZE;
   }
   return rounded;
}

unsigned int ssdWriteAdjust(int pLength){
   if (!ssdwritedirect) return pLength;
   return pageRounder(pLength);
}

void setSsdWriteDirect(unsigned int pValue){ ssdwritedirect = 0;  if (pValue) ssdwritedirect = 1;}
/*
 * Helper functions
 */
int getWriteFdByExtent(Extent* pExtent)
{
    errno=0;
    int fd = -1;
    pthread_mutex_lock(&ssdWriteFdMutex);
    auto it = Serial2ssdWriteFd.find(pExtent->serial);
    if (it !=  Serial2ssdWriteFd.end() ){
       fd = it->second; //second element is the fd
       pthread_mutex_unlock(&ssdWriteFdMutex);
       return fd;
    }
    LOG(bb,debug) << "Need to open write device by Serial="<<pExtent->serial<<".  In getWriteFdByExtent ExtentInfo: "<<*pExtent;
    pthread_mutex_unlock(&ssdWriteFdMutex); //give up the lock in case some other thread needs it
    string l_disk = getDeviceBySerial(pExtent->serial);
    if (!ssdwritedirect){
      threadLocalTrackSyscallPtr->nowTrack(TrackSyscall::SSDopenwriteNOTdirect, l_disk.c_str(),__LINE__);
      fd = ::open(l_disk.c_str(), O_WRONLY);
      threadLocalTrackSyscallPtr->clearTrack();
      LOG(bb,info) << "OPEN O_WRONLY " << l_disk.c_str()<<" fd=" << fd << (fd==-1 ? (  string(strerror(errno)) +":"+to_string(errno) ) : " ");
    }
    else{
      threadLocalTrackSyscallPtr->nowTrack(TrackSyscall::SSDopenwritedirect, l_disk.c_str(),__LINE__);
      fd = ::open(l_disk.c_str(), O_WRONLY | O_DIRECT );
      threadLocalTrackSyscallPtr->clearTrack();
      LOG(bb,info) << "OPEN O_WRONLY | O_DIRECT " << l_disk.c_str()<<" fd=" << fd << (fd==-1 ? (  string(strerror(errno)) +":"+to_string(errno) ) : " ");
    }

    if (fd < 0) return fd;
    pthread_mutex_lock(&ssdWriteFdMutex);
    Serial2ssdWriteFd[pExtent->serial]=fd;
    pthread_mutex_unlock(&ssdWriteFdMutex);
    return fd;
}

void initGovernors()
{
    l_Governors_Inited = 1;

    uint32_t l_SSD_Read_Governor_Value = config.get(process_whoami + ".SSDReadGovernor", DEFAULT_SSD_READ_GOVERNOR);
    if (l_SSD_Read_Governor_Value)
    {
        sem_init(&l_SSD_Read_Governor, 0, l_SSD_Read_Governor_Value);
        l_SSD_Read_Governor_Active = 1;
    }

    uint32_t l_SSD_Write_Governor_Value = config.get(process_whoami + ".SSDWriteGovernor", DEFAULT_SSD_WRITE_GOVERNOR);
    if (l_SSD_Write_Governor_Value)
    {
        sem_init(&l_SSD_Write_Governor, 0, l_SSD_Write_Governor_Value);
        l_SSD_Write_Governor_Active = 1;
    }

    return;
}

/*
 * Member methods
 */
void BBIO::closeAllFileHandles()
{
    return;
}

filehandle* BBIO::getFileHandle(const uint32_t pFileIndex)
{
    return (filehandle*)0;
}

uint32_t BBIO::getNumWritesNoSync(const uint32_t pFileIndex)
{
    return 0;
}

size_t BBIO::getTotalSizeWritesNoSync(const uint32_t pFileIndex)
{
    return 0;
}

int BBIO::getReadFdByExtent(Extent* pExtent)
{
    errno=0;
    int fd = -1;
    pthread_mutex_lock(&ssdReadFdMutex);
    auto it = Serial2ssdReadFd.find(pExtent->serial);
    if (it !=  Serial2ssdReadFd.end() ) {
       fd = it->second; //second element is the fd
       pthread_mutex_unlock(&ssdReadFdMutex);
       return fd;
    }
    LOG(bb,debug) << "Need to open read device by Serial=" <<pExtent->serial<<".  In BBIO::getReadFdByExtent ExtentInfo: "<<*pExtent;
    pthread_mutex_unlock(&ssdReadFdMutex);  //give up the lock in case some other thread needs it
    string l_disk = getDeviceBySerial(pExtent->serial);
    threadLocalTrackSyscallPtr->nowTrack(TrackSyscall::SSDopenreaddirect, l_disk.c_str(),__LINE__);
    fd = ::open(l_disk.c_str(), O_RDONLY | O_DIRECT);
    threadLocalTrackSyscallPtr->clearTrack();
    LOG(bb,info) << "OPEN O_RDONLY | O_DIRECT " << l_disk.c_str()<<" fd=" << fd << (fd==-1 ? strerror(errno) : " ");
    if (fd < 0) return fd;
    pthread_mutex_lock(&ssdReadFdMutex);
    Serial2ssdReadFd[pExtent->serial]=fd;
    pthread_mutex_unlock(&ssdReadFdMutex);
    return fd;
}

void BBIO::incrNumWritesNoSync(const uint32_t pFileIndex, const uint32_t pValue)
{
    return;
}

void BBIO::incrTotalSizeWritesNoSync(const uint32_t pFileIndex, const size_t pValue)
{
    return;
}

int BBIO::performIO(LVKey& pKey, Extent* pExtent)
{
    int rc = 0;

    const ssize_t BLKSIZE = 65536-1;
    int ssd_fd = -1;
    off_t offset_src;
    off_t offset_dst;
    uint64_t l_Time;
    ssize_t bytesRead;
    ssize_t bytesWritten;
    size_t count = pExtent->len;

    FL_Write6(FLXfer, BBIOPerformIO, "BBIO::performIO.  Extent=%p  Length=%ld  Start=%lx  LBAStart=%lx  Flags=0x%lx", (uint64_t)pExtent, (uint64_t)count, pExtent->start, pExtent->lba.start, pExtent->flags, 0);

    // If initial call, initialize the SSD read/write governors
    if (!l_Governors_Inited)
    {
        initGovernors();
    }

    // Perform the I/O
    if (pExtent->flags & BBI_TargetSSD)
    {
        if ( __glibc_likely(!pExtent->isCP_Transfer()) )
        {
            // Target is SSD...
            unlockTransferQueue(&pKey, "performIO - Before perform I/O to SSD for extent");

            bool l_ForceSSDWriteError = (g_ForceSSDWriteError ? ++l_Total_SSD_Writes >= g_ForceSSDWriteError ? true : false : false);
            int l_PostToReadGovernor = 0;
            try
            {
                LOG(bb,debug) << "Copying to SSD from sourceindex=" << pExtent->sourceindex;

                ssd_fd = getWriteFdByExtent(pExtent);
                if (ssd_fd < 0)
                {
                    FL_Write(FLXfer, SSDOpenFailed, "Opening the SSD for O_WRONLY failed.  Extent=%p, rc=%ld, errno=%ld", (uint64_t)pExtent, ssd_fd, errno, 0);
                    throw runtime_error(string("Unable to open the SSD for writing.  errno=") + to_string(errno));
                }

                offset_src = pExtent->start;
                offset_dst = pExtent->lba.start;

                LOG(bb,debug) << "Starting transfer to ssd fd=" << setw << ssd_fd \
                              << " from pfs " << transferDef->files[pExtent->sourceindex] \
                              << std::hex << std::uppercase << setfill('0') \
                              << " src offset=0x" << setw(8) << offset_src \
                              << " dst offset=0x" << setw(8) << offset_dst \
                              << " size=0x" << setw(8) << count \
                              << setfill(' ') << std::nouppercase << std::dec;
                while ((!rc) && count > 0)
                {
                    setupTransferBuffer(pExtent->sourceindex);

                    if (l_SSD_Read_Governor_Active)
                    {
                        sem_wait(&l_SSD_Read_Governor);
                        l_PostToReadGovernor = 1;
                    }

                    FL_Write6(FLXfer, PREAD_PFS, "Extent %p, reading from target index %ld into %p, len=%ld at offset 0x%lx", (uint64_t)pExtent, pExtent->sourceindex, (uint64_t)threadTransferBuffer, MIN(transferBufferSize, count), offset_src, 0);

                    transferDef->preProcessRead(l_Time);
                    //pread is bbio::pread derived
                    bytesRead = pread(pExtent->sourceindex, threadTransferBuffer, MIN(transferBufferSize, count), offset_src);
                    transferDef->postProcessRead(pExtent->sourceindex, l_Time);

                    if (l_PostToReadGovernor)
                    {
                        l_PostToReadGovernor = 0;
                        sem_post(&l_SSD_Read_Governor);
                    }

                    FL_Write(FLXfer, PREAD_PFSCMP, "Extent %p, reading from target index %ld.  bytesRead=%ld errno=%ld", (uint64_t)pExtent, pExtent->sourceindex, bytesRead,errno);

                    if ( __glibc_likely(bytesRead >= 0) )
                    {
                        if (offset_dst < 1024*1024)
                        {
                            // last ditch sanity check
                            throw runtime_error(string("Extent offset into SSD too low.  Offset=") + to_string(offset_dst));
                        }

                        FL_Write(FLXfer, PWRITE_SSD, "Extent %p, writing to SSD.  File descriptor %ld, length %ld at offset 0x%lx", (uint64_t)pExtent, ssd_fd, bytesRead, offset_dst);

                        threadLocalTrackSyscallPtr->nowTrack(TrackSyscall::SSDpwritesyscall, ssd_fd,__LINE__, ssdWriteAdjust(bytesRead),offset_dst);
                        transferDef->preProcessWrite(l_Time);
                        bytesWritten = (!l_ForceSSDWriteError) ? ::pwrite(ssd_fd, threadTransferBuffer, ssdWriteAdjust(bytesRead), offset_dst) : -1;
                        transferDef->postProcessWrite(pExtent->sourceindex, l_Time);
                        threadLocalTrackSyscallPtr->clearTrack();

                        if ( __glibc_likely(bytesWritten >= 0))
                        {
                            FL_Write6(FLXfer, PWRITE_SSDCMP, "Extent %p, write file descriptor %ld complete.  rc=%ld offset_dst=0x%lx sourceindex=%ld", (uint64_t)pExtent, ssd_fd, bytesWritten, offset_dst, pExtent->sourceindex, 0);
                            if (bytesWritten > bytesRead) bytesWritten = bytesRead;  //fixup for page pad
                            offset_src += bytesWritten;
                            offset_dst += bytesWritten;
                            count      -= bytesWritten;
                        }
                        else
                        {
                            stringstream errorText;
                            FL_Write6(FLXfer, PWRITE_SSDFAIL, "Extent %p, write to remote SSD failed. fd=%ld  offset_dst=%ld  bytesWritten=%ld  errno=%ld", (uint64_t)pExtent, ssd_fd, offset_dst, bytesWritten, errno, 0);
                            rc = -1;
                            errorText << "Write to remote SSD failed";
                            LOG_ERROR_TEXT_ERRNO_AND_RAS(errorText, (!l_ForceSSDWriteError) ? errno : 5, bb.sc.pwrite.ssd);
                            bberror << err("rc", rc);
                        }
                    }
                    else
                    {
                        FL_Write(FLXfer, PREAD_PFSFAIL, "Extent %p, read from PFS file failed.  sourceindex=%ld  offset_src=%ld  bytesRead=%ld", (uint64_t)pExtent, pExtent->sourceindex, offset_src, bytesRead);
                        rc = -1;
                        // BBIO subclass generated bberror and RAS
                    }
                }

                if (!rc)
                {
                    LOG(bb,debug) << "Transfer complete to ssd fd=" << setw(3) << ssd_fd << " from pfs " << transferDef->files[pExtent->sourceindex];
                }
            }
            catch (ExceptionBailout& e) { }
            catch (exception& e)
            {
                rc = -1;
                LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
            }

            if (l_PostToReadGovernor)
            {
                l_PostToReadGovernor = 0;
                sem_post(&l_SSD_Read_Governor);
            }

            lockTransferQueue(&pKey, "performIO - After perform I/O to SSD for extent");
        }
        else
        {
            LOG(bb,info) << "BBIO: No data needs to be transferred to the SSD from " << transferDef->files[pExtent->sourceindex];
        }
    }
    else if (pExtent->flags & BBI_TargetPFS)
    {
        // Target is PFS...
        size_t ssdReadCount;
        off_t ssdReadOffset;
        ssize_t l_Length;
        char* l_Buffer;

        if (!pExtent->isCP_Transfer())
        {
            // Target is PFS...
            unlockTransferQueue(&pKey, "performIO - Before perform I/O to PFS for extent");

            bool l_ForceSSDReadError = (g_ForceSSDReadError ? ++l_Total_SSD_Reads >= g_ForceSSDReadError ? true : false : false);
            int l_PostToWriteGovernor = 0;
            try
            {
                LOG(bb,debug) << "Copying to targetindex=" << pExtent->targetindex << " from SSD";

                ssd_fd = getReadFdByExtent(pExtent);
                if (ssd_fd < 0)
                    throw runtime_error(string("Unable to open the SSD for reading.  errno=") + to_string(errno));

                offset_src = pExtent->lba.start;
                offset_dst = pExtent->start;

                LOG(bb,debug) << "Starting transfer from ssd fd=" << setw(3) << ssd_fd \
                             << " to pfs " << transferDef->files[pExtent->targetindex] \
                             << std::hex << std::uppercase << setfill('0') \
                             << " src offset=0x" << setw(8) << offset_src \
                             << " dst offset=0x" << setw(8) << offset_dst \
                             << " size=0x" << setw(8) << count \
                             << setfill(' ') << std::nouppercase << std::dec;

                while ((!rc) && count > 0)
                {
                    setupTransferBuffer(pExtent->targetindex);

                    ssdReadCount  = MIN(transferBufferSize, (count+BLKSIZE)&(~BLKSIZE));
                    ssdReadOffset = offset_src & (~BLKSIZE);

                    FL_Write6(FLXfer, PREAD_SSD, "Reading from file descriptor %ld into %p, len=%ld at offset 0x%lx (pre-adjust was len=%ld offset=0x%lx)", ssd_fd, (uint64_t)threadTransferBuffer, ssdReadCount, ssdReadOffset, count, offset_src);

                    threadLocalTrackSyscallPtr->nowTrack(TrackSyscall::SSDpreadsyscall, ssd_fd,__LINE__, ssdReadCount,ssdReadOffset);
                    transferDef->preProcessRead(l_Time);
                    bytesRead = (!l_ForceSSDReadError) ? ::pread(ssd_fd, threadTransferBuffer, ssdReadCount, ssdReadOffset) : -1;
                    transferDef->postProcessRead(pExtent->sourceindex, l_Time);
                    threadLocalTrackSyscallPtr->clearTrack();

                    if ( __glibc_likely(bytesRead >= 0) )
                    {
                        FL_Write6(FLXfer, PREAD_DATA, "Data bytes read from SSD: %lx %lx %lx %lx %lx %lx", ((uint64_t*)threadTransferBuffer)[0], ((uint64_t*)threadTransferBuffer)[1], ((uint64_t*)threadTransferBuffer)[2], ((uint64_t*)threadTransferBuffer)[3], ((uint64_t*)threadTransferBuffer)[4], ((uint64_t*)threadTransferBuffer)[5]);

                        l_Buffer = &threadTransferBuffer[offset_src&BLKSIZE];
                        l_Length = MIN((ssize_t)count, bytesRead - (offset_src&BLKSIZE));

                        if (l_SSD_Write_Governor_Active)
                        {
                            sem_wait(&l_SSD_Write_Governor);
                            l_PostToWriteGovernor = 1;
                        }

                        FL_Write6(FLXfer, PWRITE_PFS, "Extent %p, writing to target index %ld into %p, len=%ld at offset 0x%lx", (uint64_t)pExtent, pExtent->targetindex, (uint64_t)l_Buffer, l_Length, offset_dst, 0);

                        transferDef->preProcessWrite(l_Time);
                        // bbio::pwrite derived
                        bytesWritten = pwrite(pExtent->targetindex, l_Buffer, l_Length, offset_dst);
                        transferDef->postProcessWrite(pExtent->sourceindex, l_Time);

                        if (l_PostToWriteGovernor)
                        {
                            l_PostToWriteGovernor = 0;
                            sem_post(&l_SSD_Write_Governor);
                        }

                        if ( __glibc_likely(bytesWritten >= 0) )
                        {
                            FL_Write6(FLXfer, PWRITE_PFSCMP, "Extent %p, write to PFS complete.  Target index=%ld, offset=0x%lx, length=%ld, bytes_written=%ld", (uint64_t)pExtent, pExtent->targetindex, l_Length, offset_dst, bytesWritten, 0);
                            offset_src += bytesWritten;
                            offset_dst += bytesWritten;
                            count      -= bytesWritten;
                        }
                        else
                        {
                            FL_Write(FLXfer, PWRITE_PFSFAIL, "Extent %p, write to PFS file failed.  targetindex=%ld  offset_dst=%ld  bytesWritten=%ld", (uint64_t)pExtent, pExtent->targetindex, offset_dst, bytesWritten);
                            rc = -1;
                            // BBIO subclass generated bberror and RAS
                        }

                    }
                    else // !(bytesRead>=0)
                    {
                        stringstream errorText;
                        FL_Write6(FLXfer, PREAD_SSDFAIL, "Extent %p, read from remote SSD failed.  fd=%ld  offset_src=%ld  bytesRead=%ld  errno=%ld", (uint64_t)pExtent, ssd_fd, offset_src, bytesRead, errno, 0);
                        rc = -1;
                        errorText << "Read from remote SSD failed";
                        LOG_ERROR_TEXT_ERRNO_AND_RAS(errorText, (!l_ForceSSDReadError) ? errno : 5, bb.sc.pread.ssd);
                        bberror << err("rc", rc);
                    }
                }
            }
            catch (ExceptionBailout& e) { }
            catch (exception& e)
            {
                rc = -1;
                LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
            }

            if (l_PostToWriteGovernor)
            {
                l_PostToWriteGovernor = 0;
                sem_post(&l_SSD_Write_Governor);
            }

            lockTransferQueue(&pKey, "performIO - After perform I/O to PFS for extent");

            if (!rc)
            {
                // Determine if we should do a periodic fsync...
                uint32_t l_FileIndex = pExtent->targetindex;
                incrNumWritesNoSync(l_FileIndex);
                incrTotalSizeWritesNoSync(l_FileIndex, pExtent->len);
                uint32_t l_NumberOfWrites = getNumWritesNoSync(l_FileIndex);
                size_t l_SizeOfWrites = getTotalSizeWritesNoSync(l_FileIndex);
                if ((getWritesBetweenSyncs() && l_NumberOfWrites >= getWritesBetweenSyncs()) || (getSizeBetweenSyncs() && l_SizeOfWrites >= getSizeBetweenSyncs()))
                {
                    // NOTE Set the counts to zero before the fsync so other threads
                    //      won't schedule another periodic fsync for this target file...
                    setNumWritesNoSync(l_FileIndex, 0);
                    setTotalSizeWritesNoSync(l_FileIndex, 0);
                    unlockTransferQueue(&pKey, "performIO - Before periodic fsync to PFS");
                    try
                    {
                        LOG(bb,info) << "Periodic PFS fsync start: targetindex=" << l_FileIndex << ", # writes " << l_NumberOfWrites << ", size of writes " << l_SizeOfWrites << ", triggered by extent " << *pExtent;
                        FL_Write(FLTInf2, PSYNC_PFS, "Performing periodic PFS fsync.  Extent %p, Target index=%ld", (uint64_t)pExtent, pExtent->targetindex, 0, 0);
                        transferDef->preProcessSync(l_Time);
                        fsync(l_FileIndex);
                        transferDef->postProcessSync(l_FileIndex, l_Time);
                        FL_Write(FLTInf2, PSYNC_PFSCMP, "Performed periodic PFS fsync.  Extent %p, Target index=%ld", (uint64_t)pExtent, pExtent->targetindex, 0, 0);
                        LOG(bb,info) << "Periodic PFS fsync end: targetindex=" << l_FileIndex;
                    }
                    catch (ExceptionBailout& e) { }
                    catch (exception& e)
                    {
                        rc = -1;
                        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
                    }
                    lockTransferQueue(&pKey, "performIO - After periodic fsync to PFS");
                }
                LOG(bb,debug) << "Transfer complete to pfs " << transferDef->files[l_FileIndex] << ", target index " << l_FileIndex \
                              << " from ssd fd " << setw(3) << ssd_fd << ", size " << pExtent->len \
                              << ", numWritesNoSync " << getNumWritesNoSync(l_FileIndex) << ", totalSizeWritesNoSync " << getTotalSizeWritesNoSync(l_FileIndex);
            }

        }
        else
        {
            LOG(bb,info) << "BBIO: No data needs to be transferred from the SSD to " << transferDef->files[pExtent->targetindex];
        }
    }
    else
    {
        LOG(bb,error) << "BBIO: Unknown extent flag specification 0x" << std::hex << std::uppercase << setfill('0') \
                      << pExtent->flags << setfill(' ') << std::nouppercase << std::dec;
    }

    return rc;
}

void BBIO::setNumWritesNoSync(const uint32_t pFileIndex, const uint32_t pValue)
{
    return;
}

void BBIO::setTotalSizeWritesNoSync(const uint32_t pFileIndex, const size_t pValue)
{
    return;
}
