/*-----------------------------------------------------------------------/
/  Low level disk interface modlue include file   (C)ChaN, 2013          /
/-----------------------------------------------------------------------*/

#ifndef FATFS_PARTICLE_SDSPI_DRIVER
#define FATFS_PARTICLE_SDSPI_DRIVER

#ifndef __cplusplus
#error "SDSPIDriver must be included only in C++"
#else

#include <FatFs/FatFs.h>
#include "trampoline.h"
#include <mutex>

/* MMC/SD command */
#define CMD0	(0)			/* GO_IDLE_STATE */
#define CMD1	(1)			/* SEND_OP_COND (MMC) */
#define	ACMD41	(0x80+41)	/* SEND_OP_COND (SDC) */
#define CMD8	(8)			/* SEND_IF_COND */
#define CMD9	(9)			/* SEND_CSD */
#define CMD10	(10)		/* SEND_CID */
#define CMD12	(12)		/* STOP_TRANSMISSION */
#define ACMD13	(0x80+13)	/* SD_STATUS (SDC) */
#define CMD16	(16)		/* SET_BLOCKLEN */
#define CMD17	(17)		/* READ_SINGLE_BLOCK */
#define CMD18	(18)		/* READ_MULTIPLE_BLOCK */
#define CMD23	(23)		/* SET_BLOCK_COUNT (MMC) */
#define	ACMD23	(0x80+23)	/* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24	(24)		/* WRITE_BLOCK */
#define CMD25	(25)		/* WRITE_MULTIPLE_BLOCK */
#define CMD32	(32)		/* ERASE_ER_BLK_START */
#define CMD33	(33)		/* ERASE_ER_BLK_END */
#define CMD38	(38)		/* ERASE */
#define CMD55	(55)		/* APP_CMD */
#define CMD58	(58)		/* READ_OCR */

#ifndef HOSS_TIMEOUT_CHECKER
#define HOSS_TIMEOUT_CHECKER
class TimeoutChecker {
	uint32_t _start;
	uint32_t _duration;
	uint32_t _end;
public:
	TimeoutChecker(uint32_t timeoutMillis) : _start(0), _duration(timeoutMillis), _end(0) { start(); }
	operator int() const { uint32_t now = millis(); return _start < _end ? now > _end : now > _end && now < _start; }
	void start() { _start = millis(); _end = _start + _duration; }
};
#endif

template<typename Pin>
class SDSPIDriver : public FatFsDriver
{
	LOG_CATEGORY("fatfs_particle.sdspidriver");
private:
	SPIClass* _spi;
	uint16_t _cs;
	Pin _cd;
	bool _cd_enabled;
	uint8_t _cd_active_state;
	Pin _wp;
	bool _wp_enabled;
	uint8_t _wp_active_state;
	volatile uint32_t _high_speed_clock;
	volatile uint32_t _low_speed_clock;
	volatile uint32_t _active_clock;
#if PLATFORM_THREADING
	std::mutex* _mutex;
#endif
	volatile DSTATUS _status;
	volatile BYTE _cardType;
	volatile bool _busy;
	volatile bool _busy_check;

	void assertCS() { digitalWrite(_cs, LOW); }
	void deassertCS() { digitalWrite(_cs, HIGH); }

	void activateLowSpeed() { _active_clock = _low_speed_clock; }
	void activateHighSpeed() { _active_clock = _high_speed_clock; }

	void setSPI()
	{
		_spi->begin(SPI_MODE_MASTER, _cs);
		_spi->setClockSpeed(_active_clock, HZ);
		_spi->setDataMode(SPI_MODE0);
		_spi->setBitOrder(MSBFIRST);
	}

	/* Send multiple byte */
	void xmit_spi_multi (
		const BYTE *buff,	/* Pointer to the data */
		UINT btx			/* Number of bytes to send (even number) */
	)
	{
#if PLATFORM_THREADING
 #ifdef SYSTEM_VERSION_060
		//use a queue to signal because the firmware implementation at the time of writing
		//checks to use the ISR version of put when appropriate
		os_queue_t signal;
		os_queue_create(&signal, sizeof(void*), 1, nullptr);
		void* result;
 #else
		std::mutex signal;
 #endif

		invoke_trampoline([&](HAL_SPI_DMA_UserCallback callback){

 #ifndef SYSTEM_VERSION_060
			signal.lock();
 #endif
			_spi->transfer((BYTE*)buff, nullptr, btx, callback);

 #ifdef SYSTEM_VERSION_060
			os_queue_take(signal, &result, CONCURRENT_WAIT_FOREVER, nullptr);
			os_queue_destroy(signal, nullptr);
			signal = nullptr;
 #else
			signal.lock();
			signal.unlock(); //superfluous, but...but...
 #endif
		}, [&]() {
 #ifdef SYSTEM_VERSION_060
			os_queue_put(signal, result, 0, nullptr);
 #else
			signal.unlock();
 #endif
		});
#else
		//DMA not working on core
		for(size_t i = 0; i < btx; i++)
			_spi->transfer((BYTE)buff[i]);
#endif
	}

	void rcvr_spi_multi (
		BYTE *buff,		/* Pointer to data buffer */
		UINT btr		/* Number of bytes to receive (even number) */
	)
	{
		/* Read multiple bytes, send 0xFF as dummy */
		memset(buff, 0xff, btr);

#if PLATFORM_THREADING
 #ifdef SYSTEM_VERSION_060
		//use a queue to signal because the firmware implementation at the time of writing
		//checks to use the ISR version of put when appropriate
		os_queue_t signal;
		os_queue_create(&signal, sizeof(void*), 1, nullptr);
		void* result;
 #else
		std::mutex signal;
 #endif

		invoke_trampoline([&](HAL_SPI_DMA_UserCallback callback){

 #ifndef SYSTEM_VERSION_060
			signal.lock();
 #endif
			_spi->transfer(buff, buff, btr, callback);

 #ifdef SYSTEM_VERSION_060
			os_queue_take(signal, &result, CONCURRENT_WAIT_FOREVER, nullptr);
			os_queue_destroy(signal, nullptr);
			signal = nullptr;
 #else
			signal.lock();
			signal.unlock(); //superfluous, but...but...
 #endif
		}, [&]() {
 #ifdef SYSTEM_VERSION_060
			os_queue_put(signal, result, 0, nullptr);
 #else
			signal.unlock();
 #endif
		});
#else
		//DMA not working on core
		for(size_t i = 0; i < btr; i++)
			buff[i] = _spi->transfer(0xff);
#endif
	}

	BYTE send_cmd (		/* Return value: R1 resp (bit7==1:Failed to send) */
			BYTE cmd,		/* Command index */
			DWORD arg		/* Argument */
		)
		{
			BYTE n, res;

			if(!wait_ready(10))
				LOG(ERROR, "SD: wait_ready before cmd failed");

			if (cmd & 0x80) {	/* Send a CMD55 prior to ACMD<n> */
				cmd &= 0x7F;
				res = send_cmd(CMD55, 0);
				if (res > 1) {
					LOG(TRACE, "SD: CMD55 response 0x%x", res);
					return res;
				}
			}

			/* Select the card and wait for ready except to stop multiple block read */
			if (cmd != CMD12) {
				deselect();
				if (!select()) return 0xFF;
			}

			/* Send command packet */
			xmit_spi(0x40 | cmd);				/* Start + command index */
			xmit_spi((BYTE)(arg >> 24));		/* Argument[31..24] */
			xmit_spi((BYTE)(arg >> 16));		/* Argument[23..16] */
			xmit_spi((BYTE)(arg >> 8));			/* Argument[15..8] */
			xmit_spi((BYTE)arg);				/* Argument[7..0] */
			n = 0x01;							/* Dummy CRC + Stop */
			if (cmd == CMD0) n = 0x95;			/* Valid CRC for CMD0(0) */
			if (cmd == CMD8) n = 0x87;			/* Valid CRC for CMD8(0x1AA) */
			xmit_spi(n);

			/* Receive command resp */
			if (cmd == CMD12) {
				xmit_spi(0xFF);					/* Diacard following one byte when CMD12 */
			}

			n = 10;								/* Wait for response (10 bytes max) */
			do {
				res = xmit_spi(0xFF);
			} while ((res & 0x80) && --n);

			return res;							/* Return received response */
		}

	BYTE xmit_spi(const BYTE b)
	{
		BYTE result = _spi->transfer(b);
		return result;
	}

	int wait_ready (	/* 1:Ready, 0:Timeout */
		UINT wt			/* Timeout [ms] */
	)
	{
		BYTE d;

		TimeoutChecker timeout(wt);

		do {
			d = xmit_spi(0xFF);
	//		if(d != 0xff)
	////			delay(1);
	//			os_thread_yield();
		} while (d != 0xFF && !timeout);	/* Wait for card goes ready or timeout */
		if (d == 0xFF) {
//			LOG(TRACE, "wait_ready: OK");
		} else {
//			LOG(TRACE, "wait_ready: timeout");
			LOG(ERROR, "SD: wait_ready timeout");
		}
		return (d == 0xFF) ? 1 : 0;
	}

	void deselect (void)
	{

		deassertCS();				/* CS = H */
		_spi->transfer(0xFF);		/* Dummy clock (force DO hi-z for multiple slave SPI) */
//		LOG(TRACE, "deselect: ok");
	}

	int select (void)	/* 1:OK, 0:Timeout */
	{
		assertCS();
		xmit_spi(0xFF);	/* Dummy clock (force DO enabled) */

		if (wait_ready(100)) {
//			LOG(TRACE, "select: OK");
			return 1;	/* OK */
		}
		LOG(TRACE, "select: no");
		deselect();
		return 0;		/* Timeout */
	}

	int xmit_datablock (	/* 1:OK, 0:Failed */
		const BYTE *buff,	/* Ponter to 512 byte data to be sent */
		BYTE token			/* Token */
	)
	{
		BYTE resp;

//		LOG(TRACE, "xmit_datablock: inside");

		if (!wait_ready(100)) {
			LOG(TRACE, "xmit_datablock: not ready");
			return 0;		/* Wait for card ready */
		}
//		LOG(TRACE, "xmit_datablock: ready");

		xmit_spi(token);					/* Send token */
		if (token != 0xFD) {				/* Send data if token is other than StopTran */
			xmit_spi_multi(buff, 512);		/* Data */
			xmit_spi(0xFF); xmit_spi(0xFF); /* Dummy CRC */
			resp = xmit_spi(0xFF);			/* Receive data resp */
			if ((resp & 0x1F) != 0x05)		/* Function fails if the data packet was not accepted */
				return 0;
		}
		return 1;
	}

	int rcvr_datablock (	/* 1:OK, 0:Error */
		BYTE *buff,			/* Data buffer */
		UINT btr			/* Data block length (byte) */
	)
	{
		BYTE token;

		TimeoutChecker timeout(200);
		do {							// Wait for DataStart token in timeout of 200ms
			token = xmit_spi(0xFF);
		} while ((token == 0xFF) && !timeout);
		if (token != 0xFE) {
			LOG(TRACE, "rcvr_datablock: token != 0xFE");
			return 0;					// Function fails if invalid DataStart token or timeout
		}

		rcvr_spi_multi(buff, btr);		// Store trailing data to the buffer
		xmit_spi(0xFF);					// Discard CRC
		return 1;						// Function succeeded
	}

	void lock()
	{
#if PLATFORM_THREADING
		if(_mutex != nullptr)
			_mutex->lock();
#endif
		_busy = true;
		_busy_check = true;
		setSPI();
	}

	void unlock()
	{
		_busy = false;
#if PLATFORM_THREADING
		if(_mutex != nullptr)
			_mutex->unlock();
#endif
	}

	friend class std::lock_guard<SDSPIDriver<Pin>>;
public:
	SDSPIDriver() : FatFsDriver(),
		_spi(nullptr),
		_cs(0),
		_cd_enabled(false),
		_cd_active_state(HIGH),
		_wp_enabled(false),
		_wp_active_state(LOW),
		_high_speed_clock(15000000),
		_low_speed_clock(400000),
		_active_clock(_low_speed_clock),
#if PLATFORM_THREADING
		_mutex(nullptr),
#endif
		_status(STA_NOINIT),
		_cardType(0),
		_busy(false),
		_busy_check(false) {}

	virtual ~SDSPIDriver() {}

	void begin(SPIClass& spi, const uint16_t cs)
	{
		_spi = &spi;
		_cs = cs;

		pinMode(cs, OUTPUT);
		digitalWrite(cs, HIGH);
	}

#if PLATFORM_THREADING
	void begin(SPIClass& spi, const uint16_t cs, std::mutex& mutex)
	{
		begin(spi, cs);

		_mutex = &mutex;
	}
#endif

	virtual DSTATUS initialize() {
		UINT n, cmd, ty, ocr[4];
		std::lock_guard<SDSPIDriver<Pin>> lck(*this);

		activateLowSpeed();

		if (!cardPresent()) {
			return STA_NODISK;
		}

		for (n = 10; n; n--) {
			xmit_spi(0xFF);
		}

		ty = 0;
		TimeoutChecker timeout(1000);

		for(n = 0; n < 200; n++) {
			delay(1);
			send_cmd(CMD0, 0);
		}

		if (send_cmd(CMD0, 0) == 1) {				/* Put the card SPI/Idle state */
			LOG(TRACE+10, "SD: CMD0 accepted");
			timeout.start();					/* Initialization timeout = 1 sec */
			if (send_cmd(CMD8, 0x1AA) == 1) {	/* SDv2? */
				LOG(TRACE+10, "SD: CMD8 accepted");
				for (n = 0; n < 4; n++) {
					ocr[n] = xmit_spi(0xFF);	/* Get 32 bit return value of R7 resp */
				}
				if (ocr[2] == 0x01 && ocr[3] == 0xAA) {					/* Is the card supports vcc of 2.7-3.6V? */
					LOG(TRACE+10, "SD: CMD8 valid response");
					while (!timeout && send_cmd(ACMD41, 1UL << 30)) ;	/* Wait for end of initialization with ACMD41(HCS) */
					if (!timeout && send_cmd(CMD58, 0) == 0) {		/* Check CCS bit in the OCR */
						LOG(TRACE+10, "SD: CMD58 accepted");
						for (n = 0; n < 4; n++) {
							ocr[n] = xmit_spi(0xFF);
						}
						ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;	/* Card id SDv2 */
						LOG(TRACE+10, "SD: card type %d", ty);
					} else {
						if(!timeout)
							LOG(TRACE+10, "SD: CMD58 unexpected response");
					}
				} else {
					LOG(ERROR, "SD: CMD8 invalid response");
				}
			} else {	/* Not SDv2 card */
				LOG(TRACE, "SD: not an SDv2 card");
				if (send_cmd(ACMD41, 0) <= 1) 	{	/* SDv1 or MMC? */
					LOG(TRACE+10, "SD: SDv1");
					ty = CT_SD1; cmd = ACMD41;	/* SDv1 (ACMD41(0)) */
				} else {
					LOG(TRACE+10, "SD: MMCv3");
					ty = CT_MMC; cmd = CMD1;	/* MMCv3 (CMD1(0)) */
				}
				while (!timeout && send_cmd(cmd, 0));			/* Wait for end of initialization */
				if (!timeout || send_cmd(CMD16, 512) != 0) {	/* Set block length: 512 */
					LOG(ERROR+10, "SD: unexpected response to CMD16");
					ty = 0;
				}
			}
		}
		else
			LOG(ERROR, "Did not receive response to CMD0");

		if(timeout)
			LOG(ERROR, "SD: timeout on initialize");

		_cardType = ty;	/* Card type */
		deselect();

		if (ty) {			/* OK */
			_status &= ~STA_NOINIT;	/* Clear STA_NOINIT flag */
		} else {			/* Failed */
			_status = STA_NOINIT;
			LOG(ERROR, "Initialize failed");
		}

		if (!writeProtected()) {
			_status |= STA_PROTECT;
		} else {
			_status &= ~STA_PROTECT;
		}

		activateHighSpeed();

		return _status;
	}

	virtual DSTATUS status() {
		if (!cardPresent()) {
			_status = STA_NODISK;
		} else if (writeProtected()) {
			_status |= STA_PROTECT;
		} else {
			_status &= ~STA_PROTECT;
		}
		return _status;
	}

	virtual DRESULT read(BYTE* buff, DWORD sector, UINT count) {
		UINT read = 0;
		std::lock_guard<SDSPIDriver<Pin>> lck(*this);

//		LOG(TRACE, "disk_read: inside");
		if (!cardPresent() || (_status & STA_NOINIT))
			return RES_NOTRDY;

		if (!(_cardType & CT_BLOCK))
			sector *= 512;						/* LBA ot BA conversion (byte addressing cards) */

		while(count != 0) {						/* Single sector read */
			if(send_cmd(CMD17, sector) == 0)	/* READ_SINGLE_BLOCK */
			{
				if(rcvr_datablock(buff + 512 * read, 512)) {
					count--;
					read++;
				} else
					LOG(ERROR, "SD: Read failed for sector %d", sector);
			}
			else
				LOG(ERROR, "SD: CMD17 not accepted");
		}
		deselect();
		return count ? RES_ERROR : RES_OK;		/* Return result */
	}

	virtual DRESULT write(const BYTE* buff, DWORD sector, UINT count)
	{
		std::lock_guard<SDSPIDriver<Pin>> lck(*this);

//		LOG(TRACE, "disk_write: inside");
		if (!cardPresent())
			return RES_ERROR;
		if (writeProtected()) {
			LOG(TRACE, "disk_write: Write protected!!!");
			return RES_WRPRT;
		}
		if (_status & STA_NOINIT)
			return RES_NOTRDY;	/* Check drive status */

		if (_status & STA_PROTECT)
			return RES_WRPRT;	/* Check write protect */

		if (!(_cardType & CT_BLOCK))
			sector *= 512;	/* LBA ==> BA conversion (byte addressing cards) */

		if (count == 1) {	/* Single sector write */
			if ((send_cmd(CMD24, sector) == 0)	/* WRITE_BLOCK */
				&& xmit_datablock(buff, 0xFE))
				count = 0;
		} else {				/* Multiple sector write */
			if (_cardType & CT_SDC) send_cmd(ACMD23, count);	/* Predefine number of sectors */
			if (send_cmd(CMD25, sector) == 0) {	/* WRITE_MULTIPLE_BLOCK */
				do {
					if (!xmit_datablock(buff, 0xFC)) {
						break;
					}
					buff += 512;
				} while (--count);
				if (!xmit_datablock(0, 0xFD)) {	/* STOP_TRAN token */
					count = 1;
				}
			}
		}
		deselect();
		return count ? RES_ERROR : RES_OK;	/* Return result */
	}

	virtual DRESULT ioctl(BYTE cmd, void* buff)
	{
		std::lock_guard<SDSPIDriver<Pin>> lck(*this);

		DRESULT res;
		BYTE n, csd[16];
		DWORD *dp, st, ed, csize;

		if (_status & STA_NOINIT) {
			return RES_NOTRDY;	/* Check if drive is ready */
		}
		if (!cardPresent()) {
			return RES_NOTRDY;
		}

		res = RES_ERROR;

		switch (cmd) {
		case CTRL_SYNC :		/* Wait for end of internal write process of the drive */
			if (select()) res = RES_OK;
			break;

		case GET_SECTOR_COUNT :	/* Get drive capacity in unit of sector (DWORD) */
			if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
				if ((csd[0] >> 6) == 1) {	/* SDC ver 2.00 */
					csize = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
					*(DWORD*)buff = csize << 10;
				} else {					/* SDC ver 1.XX or MMC ver 3 */
					n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
					csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
					*(DWORD*)buff = csize << (n - 9);
				}
				res = RES_OK;
			}
			break;

		case GET_BLOCK_SIZE :	/* Get erase block size in unit of sector (DWORD) */
			if (_cardType & CT_SD2) {	/* SDC ver 2.00 */
				if (send_cmd(ACMD13, 0) == 0) {	/* Read SD status */
					xmit_spi(0xFF);
					if (rcvr_datablock(csd, 16)) {				/* Read partial block */
						for (n = 64 - 16; n; n--) xmit_spi(0xFF);	/* Purge trailing data */

						*(DWORD*)buff = 16UL << (csd[10] >> 4);
						res = RES_OK;
					}
				}
			} else {					/* SDC ver 1.XX or MMC */
				if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {	/* Read CSD */
					if (_cardType & CT_SD1) {	/* SDC ver 1.XX */
						*(DWORD*)buff = (((csd[10] & 63) << 1) + ((WORD)(csd[11] & 128) >> 7) + 1) << ((csd[13] >> 6) - 1);
					} else {					/* MMC */
						*(DWORD*)buff = ((WORD)((csd[10] & 124) >> 2) + 1) * (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
					}
					res = RES_OK;
				}
			}
			break;

		case CTRL_ERASE_SECTOR :	/* Erase a block of sectors (used when _USE_ERASE == 1) */
			if (!(_cardType & CT_SDC)) break;				/* Check if the card is SDC */
			if (ioctl(MMC_GET_CSD, csd)) break;	/* Get CSD */
			if (!(csd[0] >> 6) && !(csd[10] & 0x40)) break;	/* Check if sector erase can be applied to the card */
			dp = (DWORD*)buff; st = dp[0]; ed = dp[1];				/* Load sector block */
			if (!(_cardType & CT_BLOCK)) {
				st *= 512; ed *= 512;
			}
			if (send_cmd(CMD32, st) == 0 && send_cmd(CMD33, ed) == 0 && send_cmd(CMD38, 0) == 0 && wait_ready(30000))	/* Erase sector block */
				res = RES_OK;	/* FatFs does not check result of this command */
			break;

		default:
			res = RES_PARERR;
		}

		deselect();

		return res;
	}

	bool cardPresent() { return !_cd_enabled || digitalRead(_cd) == _cd_active_state; }
	bool writeProtected() { return _wp_enabled && digitalRead(_wp) == _wp_active_state; }

	uint32_t highSpeedClock() { return _high_speed_clock; }
	uint32_t lowSpeedClock() { return _low_speed_clock; }
	void highSpeedClock(uint32_t clock) { _high_speed_clock = clock; }
	void lowSpeedClock(uint32_t clock) { _low_speed_clock = clock; }
	uint32_t activeClock() { return _active_clock; }

	void enableCardDetect(const Pin& cdPin, bool activeState) { _cd = cdPin; _cd_active_state = activeState; _cd_enabled = true; }
	void enableWriteProtectDetect(const Pin& wpPin, bool activeState) { _wp = wpPin; _wp_active_state = activeState; _wp_enabled = true; }

	bool wasBusySinceLastCheck() {
		bool wasBusy;
		ATOMIC_BLOCK()
		{
			wasBusy = _busy_check;
			_busy_check = _busy;
		}
		return wasBusy;
	}
};

#ifndef FATFS_PARTICLE_PINTYPE
#define FATFS_PARTICLE_PINTYPE uint16_t
#endif

typedef SDSPIDriver<FATFS_PARTICLE_PINTYPE> FatFsSD;

#endif
#endif

