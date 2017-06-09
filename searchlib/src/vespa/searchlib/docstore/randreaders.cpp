// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "randreaders.h"
#include "summaryexceptions.h"
#include <vespa/vespalib/data/databuffer.h>
#include <vespa/fastos/file.h>

#include <vespa/log/log.h>
LOG_SETUP(".search.docstore.randreaders");

namespace search {

DirectIORandRead::DirectIORandRead(const vespalib::string & fileName)
    : _file(std::make_unique<FastOS_File>(fileName.c_str())),
      _alignment(1),
      _granularity(1),
      _maxChunkSize(0x100000)
{
    _file->EnableDirectIO();
    if (_file->OpenReadOnly()) {
        if (!_file->GetDirectIORestrictions(_alignment, _granularity, _maxChunkSize)) {
            LOG(debug, "Direct IO setup failed for file %s due to %s",
                       _file->GetFileName(), _file->getLastErrorString().c_str());
        }
    } else {
        throw SummaryException("Failed opening data file", *_file, VESPA_STRLOC);
    }
}

FileRandRead::FSP
DirectIORandRead::read(size_t offset, vespalib::DataBuffer & buffer, size_t sz)
{
    size_t padBefore(0);
    size_t padAfter(0);
    bool directio = _file->DirectIOPadding(offset, sz, padBefore, padAfter);
    buffer.clear();
    buffer.ensureFree(padBefore + sz + padAfter + _alignment - 1);
    if (directio) {
        size_t unAligned = (-reinterpret_cast<size_t>(buffer.getFree()) & (_alignment - 1));
        buffer.moveFreeToData(unAligned);
        buffer.moveDataToDead(unAligned);
    }
    // XXX needs to use pread or file-position-mutex
    _file->ReadBuf(buffer.getFree(), padBefore + sz + padAfter, offset - padBefore);
    buffer.moveFreeToData(padBefore + sz);
    buffer.moveDataToDead(padBefore);
    return FSP();
}


int64_t
DirectIORandRead::getSize()
{
    return _file->GetSize();
}


MMapRandRead::MMapRandRead(const vespalib::string & fileName, int mmapFlags, int fadviseOptions)
    : _file(std::make_unique<FastOS_File>(fileName.c_str()))
{
    _file->enableMemoryMap(mmapFlags);
    _file->setFAdviseOptions(fadviseOptions);
    if ( ! _file->OpenReadOnly()) {
        throw SummaryException("Failed opening data file", *_file, VESPA_STRLOC);
    }
}


NormalRandRead::NormalRandRead(const vespalib::string & fileName)
    : _file(std::make_unique<FastOS_File>(fileName.c_str()))
{
    if ( ! _file->OpenReadOnly()) {
        throw SummaryException("Failed opening data file", *_file, VESPA_STRLOC);
    }
}

FileRandRead::FSP
MMapRandRead::read(size_t offset, vespalib::DataBuffer & buffer, size_t sz)
{
    const char *data = static_cast<const char *>(_file->MemoryMapPtr(offset));
    assert(data != nullptr);
    assert(_file->MemoryMapPtr(offset+sz-1) != nullptr);
    vespalib::DataBuffer(data, sz).swap(buffer);
    return FSP();
}

int64_t
MMapRandRead::getSize() {
    return _file->GetSize();
}

const void *
MMapRandRead::getMapping() {
    return _file->MemoryMapPtr(0);
}

MMapRandReadDynamic::MMapRandReadDynamic(const vespalib::string &fileName, int mmapFlags, int fadviseOptions)
    : _fileName(fileName),
      _holder(),
      _mmapFlags(mmapFlags),
      _fadviseOptions(fadviseOptions),
      _lock()
{
    remap();
}

void
MMapRandReadDynamic::remap()
{
    std::unique_ptr<FastOS_File> file(new FastOS_File(_fileName.c_str()));
    file->enableMemoryMap(_mmapFlags);
    file->setFAdviseOptions(_fadviseOptions);
    if (file->OpenReadOnly()) {
        vespalib::LockGuard guard(_lock);
        if (file->GetSize() > _holder.get()->GetSize()) {
            _holder.set(file.release());
            _holder.latch();
        }
    } else {
        throw SummaryException("Failed opening data file", *file, VESPA_STRLOC);
    }
}

FileRandRead::FSP
MMapRandReadDynamic::read(size_t offset, vespalib::DataBuffer & buffer, size_t sz)
{
    FSP file(_holder.get());
    const char * data(static_cast<const char *>(file->MemoryMapPtr(offset)));
    while ((data == nullptr) || (file->MemoryMapPtr(offset+sz-1) == nullptr)) {
        // Must check that both start and end of file is mapped in.
        remap();
        file = _holder.get();
        data = static_cast<const char *>(file->MemoryMapPtr(offset));
    }
    vespalib::DataBuffer(data, sz).swap(buffer);
    return file;
}

int64_t
MMapRandReadDynamic::getSize() {
    return _holder.get()->GetSize();
}

FileRandRead::FSP
NormalRandRead::read(size_t offset, vespalib::DataBuffer & buffer, size_t sz)
{
    buffer.clear();
    buffer.ensureFree(sz);
    _file->ReadBuf(buffer.getFree(), sz, offset);
    buffer.moveFreeToData(sz);
    return FSP();
}

int64_t
NormalRandRead::getSize()
{
    return _file->GetSize();
}

}
