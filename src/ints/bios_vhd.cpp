/*
*
*  Copyright (c) 2018 Shane Krueger
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "dosbox.h"
#include "callback.h"
#include "bios.h"
#include "bios_disk.h"
#include "regs.h"
#include "mem.h"
#include "dos_inc.h" /* for Drives[] */
#include "../dos/drives.h"
#include "mapper.h"
#include "SDL.h"

/*
* imageDiskVHD supports fixed, dynamic, and differential VHD file formats
*
* Public members:  Open(), GetVHDType()
*
* Notes:
* - create instance using Open()
* - code assumes that c/h/s values stored within VHD file are accurate, and does not scan the MBR
* - VHDs with 511 byte footers are not yet supported
* - VHDX files are not supported
* - checksum values are verified; and the backup header is used if the footer cannot be found or has an invalid checksum
* - code does not prevent loading if the VHD is marked as being part of a saved state
* - code prevents loading if parent does not match correct guid
* - code does not prevent loading if parent does not match correct datestamp
* - for differencing disks, parent paths are converted to ASCII; unicode characters will cancel loading
* - differencing disks only support absolute paths on Windows platforms
*
*/

imageDiskVHD::ErrorCodes imageDiskVHD::Open(const char* fileName, const bool readOnly, imageDisk** disk) {
	return Open(fileName, readOnly, disk, 0);
}

FILE * fopen_lock(const char * fname, const char * mode, bool &readonly);
imageDiskVHD::ErrorCodes imageDiskVHD::Open(const char* fileName, const bool readOnly, imageDisk** disk, const uint8_t* matchUniqueId) {
	//validate input parameters
	if (fileName == NULL) return ERROR_OPENING;
	if (disk == NULL) return ERROR_OPENING;
	//verify that C++ didn't add padding to the structure layout
	assert(sizeof(VHDFooter) == 512);
	assert(sizeof(DynamicHeader) == 1024);
	//open file and clear C++ buffering
	bool roflag = readOnly;
	FILE* file = fopen_lock(fileName, readOnly ? "rb" : "rb+", roflag);
	if (!file) return ERROR_OPENING;
	setbuf(file, NULL);
	//check that length of file is > 512 bytes
	if (fseeko64(file, 0L, SEEK_END)) { fclose(file); return INVALID_DATA; }
	if (ftello64(file) < 512) { fclose(file); return INVALID_DATA; }
	//read footer
	if (fseeko64(file, -512L, SEEK_CUR)) { fclose(file); return INVALID_DATA; }
	uint64_t footerPosition = (uint64_t)((int64_t)ftello64(file)); /* make sure to sign extend! */
	if ((int64_t)footerPosition < 0LL) { fclose(file); return INVALID_DATA; }
	VHDFooter originalfooter;
	VHDFooter footer;
	if (fread(&originalfooter, 512, 1, file) != 1) { fclose(file); return INVALID_DATA; }
	//convert from big-endian if necessary
	footer = originalfooter;
	footer.SwapByteOrder();
	//verify checksum on footer
	if (!footer.IsValid()) {
		//if invalid, read header, and verify checksum on header
		if (fseeko64(file, 0L, SEEK_SET)) { fclose(file); return INVALID_DATA; }
		if (fread(&originalfooter, sizeof(uint8_t), 512, file) != 512) { fclose(file); return INVALID_DATA; }
		//convert from big-endian if necessary
		footer = originalfooter;
		footer.SwapByteOrder();
		//verify checksum on header
		if (!footer.IsValid()) { fclose(file); return INVALID_DATA; }
	}
	//check that uniqueId matches
	if (matchUniqueId && memcmp(matchUniqueId, footer.uniqueId, 16)) { fclose(file); return INVALID_MATCH; }
	//calculate disk size
	uint64_t calcDiskSize = (uint64_t)footer.geometry.cylinders * (uint64_t)footer.geometry.heads * (uint64_t)footer.geometry.sectors * (uint64_t)512;
    if (!calcDiskSize) { fclose(file); return INVALID_DATA; }
	//if fixed image, return plain imageDisk rather than imageDiskVFD
	if (footer.diskType == VHD_TYPE_FIXED) {
		//make sure that the image size is at least as big as the geometry
		if (calcDiskSize > footerPosition) {
			fclose(file);
			return INVALID_DATA;
		}
		*disk = new imageDisk(file, fileName, footer.geometry.cylinders, footer.geometry.heads, footer.geometry.sectors, 512, true);
		return !readOnly && roflag ? UNSUPPORTED_WRITE : OPEN_SUCCESS;
	}

	//set up imageDiskVHD here
	imageDiskVHD* vhd = new imageDiskVHD();
	vhd->footerPosition = footerPosition;
	vhd->footer = footer;
	vhd->vhdType = footer.diskType;
	vhd->originalFooter = originalfooter;
	vhd->cylinders = footer.geometry.cylinders;
	vhd->heads = footer.geometry.heads;
	vhd->sectors = footer.geometry.sectors;
	vhd->sector_size = 512;
	vhd->diskSizeK = calcDiskSize / 1024;
	vhd->diskimg = file;
	vhd->diskname = fileName;
	vhd->hardDrive = true;
	vhd->active = true;
	//use delete vhd from now on to release the disk image upon failure

	//if not dynamic or differencing, fail
	if (footer.diskType != VHD_TYPE_DYNAMIC && footer.diskType != VHD_TYPE_DIFFERENCING) {
		delete vhd;
		return UNSUPPORTED_TYPE;
	}
	//read dynamic disk header (applicable for dynamic and differencing types)
	DynamicHeader dynHeader;
	if (fseeko64(file, (off_t)footer.dataOffset, SEEK_SET)) { delete vhd; return INVALID_DATA; }
	if (fread(&dynHeader, sizeof(uint8_t), 1024, file) != 1024) { delete vhd; return INVALID_DATA; }
	//swap byte order and validate checksum
	dynHeader.SwapByteOrder();
	if (!dynHeader.IsValid()) { delete vhd; return INVALID_DATA; }
	//check block size is valid (should be a power of 2, and at least equal to one sector)
	if (dynHeader.blockSize < 512 || (dynHeader.blockSize & (dynHeader.blockSize - 1))) { delete vhd; return INVALID_DATA; }

	//if differencing, try to open parent
	if (footer.diskType == VHD_TYPE_DIFFERENCING) {

		//loop through each reference and try to find one we can open
		for (int i = 0; i < 8; i++) {
			//ignore entries with platform code 'none'
			if (dynHeader.parentLocatorEntry[i].platformCode != 0) {
				//load the platform data, if there is any
				uint64_t dataOffset = dynHeader.parentLocatorEntry[i].platformDataOffset;
				uint32_t dataLength = dynHeader.parentLocatorEntry[i].platformDataLength;
				uint8_t* buffer = 0;
				if (dataOffset && dataLength && ((uint64_t)dataOffset + dataLength) <= footerPosition) {
					if (fseeko64(file, (off_t)dataOffset, SEEK_SET)) { delete vhd; return INVALID_DATA; }
					buffer = (uint8_t*)malloc(dataLength + 2);
					if (buffer == 0) { delete vhd; return INVALID_DATA; }
					if (fread(buffer, sizeof(uint8_t), dataLength, file) != dataLength) { free(buffer); delete vhd; return INVALID_DATA; }
					buffer[dataLength] = 0; //append null character, just in case
					buffer[dataLength + 1] = 0; //two bytes, because this might be UTF-16
				}
				ErrorCodes ret = TryOpenParent(fileName, dynHeader.parentLocatorEntry[i], buffer, buffer ? dataLength : 0, &(vhd->parentDisk), dynHeader.parentUniqueId);
				if (buffer) free(buffer);
				if (vhd->parentDisk) { //if successfully opened
					vhd->parentDisk->Addref();
					break;
				}
				if (ret != ERROR_OPENING) {
					delete vhd;
					return (ErrorCodes)(ret | PARENT_ERROR);
				}
			}
		}
		//check and see if we were successful in opening a file
		if (vhd->parentDisk == 0) { delete vhd; return ERROR_OPENING_PARENT; }
	}

	//calculate sectors per block
	uint32_t sectorsPerBlock = dynHeader.blockSize / 512;
	//calculate block map size
	uint32_t blockMapSectors = ((
		((sectorsPerBlock /* 4096 sectors/block (typ) = 4096 bits required */
		+ 7) / 8) /* convert to bytes and round up to nearest byte; 4096/8 = 512 bytes required */
		+ 511) / 512); /* convert to sectors and round up to nearest sector; 512/512 = 1 sector */
	//check that the BAT is large enough for the disk
	uint32_t tablesRequired = (uint32_t)((calcDiskSize + (dynHeader.blockSize - 1)) / dynHeader.blockSize);
	if (dynHeader.maxTableEntries < tablesRequired) { delete vhd; return INVALID_DATA; }
	//check that the BAT is contained within the file
	if (((uint64_t)dynHeader.tableOffset + ((uint64_t)dynHeader.maxTableEntries * (uint64_t)4)) > footerPosition) { delete vhd; return INVALID_DATA; }

	//set remaining variables
	vhd->dynamicHeader = dynHeader;
	vhd->blockMapSectors = blockMapSectors;
	vhd->blockMapSize = blockMapSectors * 512;
	vhd->sectorsPerBlock = sectorsPerBlock;
	vhd->currentBlockDirtyMap = (uint8_t*)malloc(vhd->blockMapSize);
	if (vhd->currentBlockDirtyMap == 0) { delete vhd; return INVALID_DATA; }

	//try loading the first block
	if (!vhd->loadBlock(0)) { 
		delete vhd; 
		return INVALID_DATA; 
	}

	*disk = vhd;
	return !readOnly && roflag ? UNSUPPORTED_WRITE : OPEN_SUCCESS;
}

int iso8859_1_encode(int utf32code) {
	return ((utf32code >= 32 && utf32code <= 126) || (utf32code >= 160 && utf32code <= 255)) ? utf32code : -1;
}

//this function (1) converts data from UTF-16 to a native string for fopen (depending on the host OS), and (2) converts slashes, if necessary
bool imageDiskVHD::convert_UTF16_for_fopen(std::string &string, const void* data, const uint32_t dataLength) {
	//note: data is UTF-16 and always little-endian, with no null terminator
	//dataLength is not the number of characters, but the number of bytes

	string.reserve((size_t)(string.size() + (dataLength / 2) + 10)); //allocate a few extra bytes
	char* indata = (char*)data;
    const char* lastchar = indata + dataLength;
#if !defined (WIN32) && !defined(OS2)
	char temp[10];
	char* tempout;
	char* tempout2;
	char* tempoutmax = &temp[10];
#endif
	while (indata < lastchar) {
		//decode the character
		int utf32code = utf16le_decode((const char**)&indata, lastchar);
		if (utf32code < 0) return false;
#if defined (WIN32) || defined(OS2)
		//MSDN docs define fopen to accept strings in the windows default code page, which is typically Windows-1252
		//convert unicode string to ISO-8859, which is a subset of Windows-1252, and a lot easier to implement
		int iso8859_1code = iso8859_1_encode(utf32code);
		if (iso8859_1code < 0) return false;
		//and note that backslashes stay as backslashes on windows
		string += (char)iso8859_1code;
#else
		//backslashes become slashes on Linux
		if (utf32code == '\\') utf32code = '/';
		//Linux ext3 filenames are byte strings, typically in UTF-8
		//encode the character to a temporary buffer
		tempout = temp;
		if (utf8_encode(&tempout, tempoutmax, (unsigned int)utf32code) < 0) return false;
		//and append the byte(s) to the string
		tempout2 = temp;
		while (tempout2 < tempout) string += *tempout2++;
#endif
	}
	return true;
}

imageDiskVHD::ErrorCodes imageDiskVHD::TryOpenParent(const char* childFileName, const imageDiskVHD::ParentLocatorEntry& entry, const uint8_t* data, const uint32_t dataLength, imageDisk** disk, const uint8_t* uniqueId) {
	std::string str = "";
	const char* slashpos = NULL;

	switch (entry.platformCode) {
	case 0x57327275:
		//Unicode relative pathname (UTF-16) on Windows

#if defined (WIN32) || defined(OS2)
		/* Windows */
		slashpos = strrchr(childFileName, '\\');
#else
		/* Linux */
		slashpos = strrchr(childFileName, '/');
#endif
		if (slashpos != NULL) {
			//copy all characters up to and including the slash, to str
            for (const char* pos = (char*)childFileName; pos <= slashpos; pos++) {
				str += *pos;
			}
		}

		//convert byte order, and UTF-16 to ASCII, and change backslashes to slashes if on Linux
		if (!convert_UTF16_for_fopen(str, data, dataLength)) break;

		return imageDiskVHD::Open(str.c_str(), true, disk, uniqueId);

	case 0x57326B75:
		//Unicode absolute pathname (UTF-16) on Windows

#if defined (WIN32) || defined(OS2)
		/* nothing */
#else
		// Linux
		// Todo: convert absolute pathname to something applicable for Linux
		break;
#endif

		//convert byte order, and UTF-16 to ASCII, and change backslashes to slashes if on Linux
		if (!convert_UTF16_for_fopen(str, data, dataLength)) break;

		return imageDiskVHD::Open(str.c_str(), true, disk, uniqueId);

	case 0x4D616320:
		//Mac OS alias stored as blob
		break;
	case 0x4D616358:
		//Mac OSX file url (RFC 2396)
		break;
	}
	return ERROR_OPENING; //return ERROR_OPENING if the file does not exist, cannot be accessed, etc
}

uint8_t imageDiskVHD::Read_AbsoluteSector(uint32_t sectnum, void * data) {
	uint32_t blockNumber = sectnum / sectorsPerBlock;
	uint32_t sectorOffset = sectnum % sectorsPerBlock;
	if (!loadBlock(blockNumber)) return 0x05; //can't load block
	if (currentBlockAllocated) {
		uint32_t byteNum = sectorOffset / 8;
		uint32_t bitNum = sectorOffset % 8;
		bool hasData = currentBlockDirtyMap[byteNum] & (1 << (7 - bitNum));
		if (hasData) {
			if (fseeko64(diskimg, (off_t)(((uint64_t)currentBlockSectorOffset + blockMapSectors + sectorOffset) * 512ull), SEEK_SET)) return 0x05; //can't seek
			if (fread(data, sizeof(uint8_t), 512, diskimg) != 512) return 0x05; //can't read
			return 0;
		}
	}
	if (parentDisk) {
		return parentDisk->Read_AbsoluteSector(sectnum, data);
	}
	else {
		memset(data, 0, 512);
		return 0;
	}
}

bool is_zeroed_sector(const void* data) {
    uint32_t *p = (uint32_t*) data;
    uint8_t* q = ((uint8_t*)data + 512);
    while((void*)p < (void*)q && *p++ == 0);
    if((void*)p < (void*)q)
        return false;
    return true;
}

uint8_t imageDiskVHD::Write_AbsoluteSector(uint32_t sectnum, const void * data) {
	uint32_t blockNumber = sectnum / sectorsPerBlock;
	uint32_t sectorOffset = sectnum % sectorsPerBlock;
	if (!loadBlock(blockNumber)) return 0x05; //can't load block
	if (!currentBlockAllocated) {
        //an unallocated block is kept virtualized until zeroed
        if(is_zeroed_sector(data)) return 0;

		if (!copiedFooter) {
			//write backup of footer at start of file (should already exist, but we never checked to be sure it is readable or matches the footer we used)
			if (fseeko64(diskimg, (off_t)0, SEEK_SET)) return 0x05;
			if (fwrite(&originalFooter, sizeof(uint8_t), 512, diskimg) != 512) return 0x05;
			copiedFooter = true;
			//flush the data to disk after writing the backup footer
			if (fflush(diskimg)) return 0x05;
		}
		//calculate new location of footer, and round up to nearest 512 byte increment "just in case"
        uint64_t newFooterPosition = (((footerPosition + blockMapSize + dynamicHeader.blockSize) + 511ull) / 512ull) * 512ull;
		//attempt to extend the length appropriately first (on some operating systems this will extend the file)
		if (fseeko64(diskimg, (off_t)newFooterPosition + 512, SEEK_SET)) return 0x05;
		//now write the footer
		if (fseeko64(diskimg, (off_t)newFooterPosition, SEEK_SET)) return 0x05;
		if (fwrite(&originalFooter, sizeof(uint8_t), 512, diskimg) != 512) return 0x05;
		//save the new block location and new footer position
		uint32_t newBlockSectorNumber = (uint32_t)((footerPosition + 511ul) / 512ul);
		footerPosition = newFooterPosition;
		//clear the dirty flags for the new footer position
		for (uint32_t i = 0; i < blockMapSize; i++) currentBlockDirtyMap[i] = 0;
		//write the dirty map
		if (fseeko64(diskimg, (off_t)(newBlockSectorNumber * 512ull), SEEK_SET)) return 0x05;
		if (fwrite(currentBlockDirtyMap, sizeof(uint8_t), blockMapSize, diskimg) != blockMapSize) return 0x05;
		//flush the data to disk after expanding the file, before allocating the block in the BAT
		if (fflush(diskimg)) return 0x05;
		//update the BAT
		if (fseeko64(diskimg, (off_t)(dynamicHeader.tableOffset + (blockNumber * 4ull)), SEEK_SET)) return 0x05;
		uint32_t newBlockSectorNumberBE = SDL_SwapBE32(newBlockSectorNumber);
		if (fwrite(&newBlockSectorNumberBE, sizeof(uint8_t), 4, diskimg) != 4) return false;
		currentBlockAllocated = true;
		currentBlockSectorOffset = newBlockSectorNumber;
		//flush the data to disk after allocating a block
		if (fflush(diskimg)) return 0x05;
	}
	//current block has now been allocated
	uint32_t byteNum = sectorOffset / 8;
	uint32_t bitNum = sectorOffset % 8;
	bool hasData = currentBlockDirtyMap[byteNum] & (1 << (7 - bitNum));
	//if the sector hasn't been marked as dirty, mark it as dirty
	if (!hasData) {
		currentBlockDirtyMap[byteNum] |= 1 << (7 - bitNum);
		if (fseeko64(diskimg, (off_t)(currentBlockSectorOffset * 512ull), SEEK_SET)) return 0x05; //can't seek
		if (fwrite(currentBlockDirtyMap, sizeof(uint8_t), blockMapSize, diskimg) != blockMapSize) return 0x05;
	}
	//current sector has now been marked as dirty
	//write the sector
	if (fseeko64(diskimg, (off_t)(((uint64_t)currentBlockSectorOffset + (uint64_t)blockMapSectors + (uint64_t)sectorOffset) * 512ull), SEEK_SET)) return 0x05; //can't seek
	if (fwrite(data, sizeof(uint8_t), 512, diskimg) != 512) return 0x05; //can't write
	return 0;
}

imageDiskVHD::VHDTypes imageDiskVHD::GetVHDType(void) const {
	return footer.diskType;
}

imageDiskVHD::VHDTypes imageDiskVHD::GetVHDType(const char* fileName) {
	imageDisk* disk;
	if (Open(fileName, true, &disk)) return VHD_TYPE_NONE;
	const imageDiskVHD* vhd = dynamic_cast<imageDiskVHD*>(disk);
	VHDTypes ret = VHD_TYPE_FIXED; //fixed if an imageDisk was returned
	if (vhd) ret = vhd->footer.diskType; //get the actual type if an imageDiskVHD was returned
	delete disk;
	return ret;
}

bool imageDiskVHD::loadBlock(const uint32_t blockNumber) {
	if (currentBlock == blockNumber) return true;
	if (blockNumber >= dynamicHeader.maxTableEntries) return false;
	if (fseeko64(diskimg, (off_t)(dynamicHeader.tableOffset + (blockNumber * 4ull)), SEEK_SET)) return false;
	uint32_t blockSectorOffset;
	if (fread(&blockSectorOffset, sizeof(uint8_t), 4, diskimg) != 4) return false;
	blockSectorOffset = SDL_SwapBE32(blockSectorOffset);
	if (blockSectorOffset == 0xFFFFFFFFul) {
		currentBlock = blockNumber;
		currentBlockAllocated = false;
	}
	else {
		if (fseeko64(diskimg, (off_t)(blockSectorOffset * (uint64_t)512), SEEK_SET)) return false;
		currentBlock = 0xFFFFFFFFul;
		currentBlockAllocated = true;
		currentBlockSectorOffset = blockSectorOffset;
		if (fread(currentBlockDirtyMap, sizeof(uint8_t), blockMapSize, diskimg) != blockMapSize) return false;
		currentBlock = blockNumber;
	}
	return true;
}

imageDiskVHD::~imageDiskVHD() {
	if (currentBlockDirtyMap) {
		free(currentBlockDirtyMap);
		currentBlockDirtyMap = 0;
	}
	if (parentDisk) {
		parentDisk->Release();
		parentDisk = 0;
	}
}

void imageDiskVHD::VHDFooter::SwapByteOrder() {
	features = SDL_SwapBE32(features);
	fileFormatVersion = SDL_SwapBE32(fileFormatVersion);
	dataOffset = SDL_SwapBE64(dataOffset);
	timeStamp = SDL_SwapBE32(timeStamp);
	creatorVersion = SDL_SwapBE32(creatorVersion);
	creatorHostOS = SDL_SwapBE32(creatorHostOS);
	originalSize = SDL_SwapBE64(originalSize);
	currentSize = SDL_SwapBE64(currentSize);
	geometry.cylinders = SDL_SwapBE16(geometry.cylinders);
	diskType = (VHDTypes)SDL_SwapBE32((uint32_t)diskType);
	checksum = SDL_SwapBE32(checksum);
	//guid might need the byte order swapped also
	//however, for our purposes (comparing to the parent guid on differential disks),
	//  it doesn't matter so long as we are consistent
}

uint32_t imageDiskVHD::VHDFooter::CalculateChecksum() {
	//checksum is one's complement of sum of bytes excluding the checksum
	//because of that, the byte order doesn't matter when calculating the checksum
	//however, the checksum must be stored in the correct byte order or it will not match

	uint32_t ret = 0;
    const uint8_t* dat = (uint8_t*)this->cookie;
	uint32_t oldChecksum = checksum;
	checksum = 0;
	for (size_t i = 0; i < sizeof(VHDFooter); i++) {
		ret += dat[i];
	}
	checksum = oldChecksum;
	return ~ret;
}

bool imageDiskVHD::VHDFooter::IsValid() {
	return (
		memcmp(cookie, "conectix", 8) == 0 &&
		fileFormatVersion >= 0x00010000 &&
		fileFormatVersion <= 0x0001FFFF &&
		checksum == CalculateChecksum());
}

void imageDiskVHD::DynamicHeader::SwapByteOrder() {
	dataOffset = SDL_SwapBE64(dataOffset);
	tableOffset = SDL_SwapBE64(tableOffset);
	headerVersion = SDL_SwapBE32(headerVersion);
	maxTableEntries = SDL_SwapBE32(maxTableEntries);
	blockSize = SDL_SwapBE32(blockSize);
	checksum = SDL_SwapBE32(checksum);
	parentTimeStamp = SDL_SwapBE32(parentTimeStamp);
	for (int i = 0; i < 256; i++) {
		parentUnicodeName[i] = SDL_SwapBE16(parentUnicodeName[i]);
	}
	for (int i = 0; i < 8; i++) {
		parentLocatorEntry[i].platformCode = SDL_SwapBE32(parentLocatorEntry[i].platformCode);
		parentLocatorEntry[i].platformDataSpace = SDL_SwapBE32(parentLocatorEntry[i].platformDataSpace);
		parentLocatorEntry[i].platformDataLength = SDL_SwapBE32(parentLocatorEntry[i].platformDataLength);
		parentLocatorEntry[i].reserved = SDL_SwapBE32(parentLocatorEntry[i].reserved);
		parentLocatorEntry[i].platformDataOffset = SDL_SwapBE64(parentLocatorEntry[i].platformDataOffset);
	}
	//parent guid might need the byte order swapped also
	//however, for our purposes (comparing to the parent guid on differential disks),
	//  it doesn't matter so long as we are consistent
}

uint32_t imageDiskVHD::DynamicHeader::CalculateChecksum() {
	//checksum is one's complement of sum of bytes excluding the checksum
	//because of that, the byte order doesn't matter when calculating the checksum
	//however, the checksum must be stored in the correct byte order or it will not match

	uint32_t ret = 0;
    const uint8_t* dat = (uint8_t*)this->cookie;
	uint32_t oldChecksum = checksum;
	checksum = 0;
	for (size_t i = 0; i < sizeof(DynamicHeader); i++) {
		ret += dat[i];
	}
	checksum = oldChecksum;
	return ~ret;
}

bool imageDiskVHD::DynamicHeader::IsValid() {
	return (
		memcmp(cookie, "cxsparse", 8) == 0 &&
		headerVersion >= 0x00010000 &&
		headerVersion <= 0x0001FFFF &&
		checksum == CalculateChecksum());
}

// Computates VHD CRC on Footer/Header
uint32_t imageDiskVHD::mk_crc(uint8_t* s, uint32_t size) {
    uint32_t crc = 0;
    for(size_t i = 0; i < size; i++)
        crc += s[i];
    return ~crc;
}

// Computates VHD CHS geometry
void imageDiskVHD::mk_chs(uint64_t size, char* chs) {
    uint32_t sectors = size / 512;
    uint8_t spt, hh;
    uint16_t cth, cyls;

    if(sectors > 65535 * 16 * 255)
        sectors = 65535 * 16 * 255;

    if(sectors >= 65535 * 16 * 63) {
        spt = 255;
        hh = 16;
        cth = sectors / spt;
    }
    else {
        spt = 17;
        cth = sectors / spt;
        hh = (cth + 1023) / 1024;
        if(hh < 4) hh = 4;
        if(cth >= hh * 1024 || hh > 16) {
            spt = 31;
            hh = 16;
            cth = sectors / spt;
        }
        if(cth >= hh * 1024) {
            spt = 63;
            hh = 16;
            cth = sectors / spt;
        }
    }
    cyls = cth / hh;

    *((uint16_t*)(chs)) = SDL_SwapBE16(cyls);
    *(chs + 2) = hh;
    *(chs + 3) = spt;
}


// Default VHD Footer
unsigned char footer_head[64] = {
    0x63, 0x6F, 0x6E, 0x65, 0x63, 0x74, 0x69, 0x78, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x44, 0x42, 0x4F, 0x58, 0x00, 0x01, 0x00, 0x00,
    0x57, 0x69, 0x32, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03
};

// Default Dynamic VHD Header
unsigned char dyn_head[48] = {
    0x63, 0x78, 0x73, 0x70, 0x61, 0x72, 0x73, 0x65, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Creates a Dynamic VHD image. Returns zero for success, 1-5 in case of errors.
uint32_t imageDiskVHD::CreateDynamic(const char* filename, uint64_t size) {
    if(!filename) return 1;
    if(size < 3145728) return 2;
    // This is the Windows 11 mounter limit
    if(size > 2190433320960) return 3;

    FILE* vhd = fopen(filename, "wb");
    if(!vhd) return 4;

    // Setup main VHD structures with default values
    char* Footer = (char*)malloc(1536);
    char* Header = Footer + 512;
    memset(Footer, 0, 1536);
    memcpy(Footer, footer_head, sizeof(footer_head));
    memcpy(Header, dyn_head, sizeof(dyn_head));

    time_t T;
    time(&T);
    *((uint32_t*)(Footer + 24)) = SDL_SwapBE32((uint32_t)T - 946681200);

    srand(time(NULL));

    *((uint64_t*)(Footer + 40)) = SDL_SwapBE64((uint64_t)size);  // u64OriginalSize
    *((uint64_t*)(Footer + 48)) = SDL_SwapBE64((uint64_t)size);  // u64CurrentSize
    *((uint16_t*)(Footer + 68)) = rand();                        // UUID
    *((uint16_t*)(Footer + 70)) = rand();
    *((uint16_t*)(Footer + 72)) = rand();
    *((uint16_t*)(Footer + 74)) = rand();
    *((uint16_t*)(Footer + 76)) = rand();
    *((uint16_t*)(Footer + 78)) = rand();
    *((uint16_t*)(Footer + 80)) = rand();
    *((uint16_t*)(Footer + 82)) = rand();

    mk_chs(size, Footer + 56);

    *((uint32_t*)(Footer + 64)) = SDL_SwapBE32(mk_crc((uint8_t*)Footer, 512));

    // Footer copy
    if (fwrite(Footer, 1, 512, vhd) != 512) return 5;

    uint32_t dwMaxTableEntries = (size + ((2 << 20) - 1)) / (2 << 20);

    *((uint32_t*)(Header + 28)) = SDL_SwapBE32(dwMaxTableEntries);    // dwMaxTableEntries
    *((uint32_t*)(Header + 36)) = SDL_SwapBE32(mk_crc((uint8_t*)Header, 1024));

    // Dynamic Header
    if (fwrite(Header, 1, 1024, vhd) != 1024) return 5;

    // Creates the empty BAT (max 4MB)
    uint32_t table_size = (4 * dwMaxTableEntries + 511) / 512 * 512;  // must span sectors
    void* table = malloc(table_size);
    memset(table, 0xFF, table_size);
    if (fwrite(table, 1, table_size, vhd) != table_size) return 5;

    // Main Footer
    if(fwrite(Footer, 1, 512, vhd) != 512) return 5;

    free(Footer);
    free(table);
    fclose(vhd);

    return 0;
}

// Creates a Differencing VHD image. Returns zero for success, 1-5 in case of errors.
uint32_t imageDiskVHD::CreateDifferencing(const char* filename, const char* basename) {
    if(!filename) return 1;
    if(!basename) return 2;

    imageDiskVHD* base_vhd;
    if(Open(basename, true, (imageDisk**)&base_vhd) != OPEN_SUCCESS)
        return 3;

    FILE* vhd = fopen(filename, "wb");
    if(!vhd) return 4;

    // Clones parent's VHD structures
    char* Footer = (char*)malloc(1536);
    char* Header = Footer + 512;
    memcpy(Footer, &base_vhd->originalFooter, 512);
    base_vhd->dynamicHeader.SwapByteOrder();
    memcpy(Header, &base_vhd->dynamicHeader, 1024);

    // Updates
    memcpy(Header + 0x28, Footer + 0x44, 16); // sParentUniqueId
    memcpy(Header + 0x38, Footer + 0x18, 4);  // dwParentTimeStamp

    *((uint32_t*)(Footer + 0x3C)) = SDL_SwapBE32(4); // dwDiskType

    time_t T;
    time(&T);
    *((uint32_t*)(Footer + 24)) = SDL_SwapBE32((uint32_t)T - 946681200);

    srand(time(NULL));
    *((uint16_t*)(Footer + 68)) = rand(); // UUID
    *((uint16_t*)(Footer + 70)) = rand();
    *((uint16_t*)(Footer + 72)) = rand();
    *((uint16_t*)(Footer + 74)) = rand();
    *((uint16_t*)(Footer + 76)) = rand();
    *((uint16_t*)(Footer + 78)) = rand();
    *((uint16_t*)(Footer + 80)) = rand();
    *((uint16_t*)(Footer + 82)) = rand();

    // Blanks old CRC and computates new one
    *((uint32_t*)(Footer + 64)) = 0;
    *((uint32_t*)(Footer + 64)) = SDL_SwapBE32(mk_crc((uint8_t*)Footer, 512));

    // Footer copy
    if (fwrite(Footer, 1, 512, vhd) != 512) return 5;

    // BAT size
    uint32_t dwMaxTableEntries = SDL_SwapBE32(*(uint32_t*)(Header + 0x1C));
    uint32_t table_size = (4 * dwMaxTableEntries + 511) / 512 * 512;

    // Windows 11 wants at least the relative W2ru locator, or won't mount!
    // sParentLocatorEntries[0]
    *((uint32_t*)(Header + 0x240)) = SDL_SwapBE32(basename[1]==':'? 0x57326B75 : 0x57327275); // W2ku : W2ru
    uint32_t l_basename = strlen(basename);
    uint32_t platsize = (2 * l_basename + 511) / 512 * 512;
    *((uint32_t*)(Header + 0x244)) = SDL_SwapBE32(platsize);                  // dwPlatformDataSpace (1+ sectors)
    *((uint32_t*)(Header + 0x248)) = SDL_SwapBE32(2*l_basename);              // dwPlatformDataLength
    *((uint64_t*)(Header + 0x250)) = SDL_SwapBE64((uint64_t)1536+table_size); // u64PlatformDataOffset

    // Dynamic Header
    *((uint32_t*)(Header + 36)) = 0;
    *((uint32_t*)(Header + 36)) = SDL_SwapBE32(mk_crc((uint8_t*)Header, 1024));
    if(fwrite(Header, 1, 1024, vhd) != 1024) return 5;

    // BAT
    void* table = malloc(table_size);
    memset(table, 0xFF, table_size);
    if(fwrite(table, 1, table_size, vhd) != table_size) return 5;

    // Relative Parent Locator sector(s)
    wchar_t* w_basename = (wchar_t*)malloc(platsize);
    memset(w_basename, 0, platsize);
    for(uint32_t i = 0; i < l_basename; i++)
        // dirty hack to quickly convert ASCII -> UTF-16 *LE* and fix slashes
        w_basename[i] = SDL_SwapLE16(basename[i]=='/'? (uint16_t)'\\' : (uint16_t)basename[i]);
    if (fwrite(w_basename, 1, platsize, vhd) != platsize) return 5;
 
    // Footer copy
    if(fwrite(Footer, 1, 512, vhd) != 512) return 5;

    delete base_vhd;
    fclose(vhd);
    free(Footer);
    free(table);
    free(w_basename);

    return 0;
}
