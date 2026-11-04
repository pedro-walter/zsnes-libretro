/* generated */
extern void DSPOp0A(void);
extern void DSPOp00(void);
extern void DSPOp10(void);
extern void DSPOp04(void);
extern void DSPOp08(void);
extern void DSPOp18(void);
extern void DSPOp28(void);
extern void DSPOp0C(void);
extern void DSPOp1C(void);
extern void DSPOp02(void);
extern void DSPOp06(void);
extern void DSPOp0E(void);
extern void DSPOp01(void);
extern void DSPOp11(void);
extern void DSPOp21(void);
extern void DSPOp0D(void);
extern void DSPOp0F(void);
extern void DSPOp1D(void);
extern void DSPOp2D(void);
extern void DSPOp03(void);
extern void DSPOp13(void);
extern void DSPOp23(void);
extern void DSPOp0B(void);
extern void DSPOp1B(void);
extern void DSPOp2B(void);
extern void DSPOp14(void);
extern void SPC7_Convert_Upper(void);
extern void SPC7_Convert_Lower(void);
extern void ST010DoCommand(void);
extern void RestoreCVFrame(void);
extern void BackupCVFrame(void);
extern void SRAMChdir(void);
extern void headerhack(void);
extern void C4TransfWireFrame2(void);
extern void C4CalcWireFrame(void);
extern void C4TransfWireFrame(void);
extern void C4Op15(void);
extern void C4Op1F(void);
extern void C4Op0D(void);
extern void SDD1_init(void);
extern void SDD1_get_byte(void);
extern void placedate(void);
extern void GetTime(void);
extern void clearmem2(void);
extern void DrawWater(void);
extern void DrawBurn(void);
extern void GetHostName(void);
extern void ZFileCHDir(void);
extern void clearmem(void);
extern void Setper2exec(void);
extern void clearSPCRAM(void);
extern void IntlEHi(void);
extern void pushdir(void);
extern void PatchUsingIPS(void);
extern void popdir(void);
extern void SPC7PackIndexLoad(void);
extern void loadROM(void);
extern void CalcChecksum(void);
extern void DumpROMLoadInfo(void);
extern void SetupSramSize(void);
extern void BankCheck(void);
extern void MirrorROM(void);
extern void chip_detect(void);
extern void InitDSP(void);
extern void ZFileSystemInit(void);
extern void obtaindir(void);
extern void getcfg(void);
extern void ConvertJoyMap(void);
extern void ccmdline(void);
extern void DOScreatenewcfg(void);
extern void zl_putchar(void);
extern void zl_getchar(void);
extern void ZOpenFile(void);
extern void ZFileSeek(void);
extern void ZFileWrite(void);
extern void ZFileRead(void);
extern void ZFileDelete(void);
extern void ZCloseFile(void);
extern void ZFileTell(void);
extern void GetDate(void);
extern void ZFileGetFTime(void);
extern void ZFileMKDir(void);
extern void ZFileRMDir(void);
extern void ZFileGetDir(void);
extern void ZFileFindFirst(void);
extern void ZFileFindNext(void);
extern void Start60HZ(void);
extern void initwinvideo(void);
extern void clearwin(void);
extern void Stop60HZ(void);
extern void drawscreenwin(void);
extern void UpdateVFrame(void);
extern void GetMouseX(void);
extern void GetMouseY(void);
extern void GetMouseButton(void);
extern void GetMouseMoveX(void);
extern void GetMouseMoveY(void);
extern void Start36HZ(void);
extern void Stop36HZ(void);
extern void CheckTimers(void);
extern void sem_sleep(void);
extern void InitTCP(void);
extern void StartServerCycle(void);
extern void ServerCheckNewClient(void);
extern void acceptzuser(void);
extern void ConnectServer(void);
extern void WaitForServer(void);
extern void SendData(void);
extern void SendDataUDP(void);
extern void GetData(void);
extern void DeInitTCP(void);
extern void StopServer(void);
extern void Disconnect(void);
extern void UDPClearVars(void);
extern void UDPWait1Sec(void);
extern void UDPEnableMode(void);
extern void UDPDisableMode(void);
extern void WinErrorA2(void);
extern void WinErrorB2(void);
extern void WinErrorC2(void);
extern void ZsnesPage(void);
extern void GetLocalTime(void);
extern void zl_malloc(void);
extern void GetFilename(void);
extern void LinuxExit(void);
extern void cycleinputdevice(void);
void *const lift_ext_fns[] = {
    (void *)DSPOp0A,
    (void *)DSPOp00,
    (void *)DSPOp10,
    (void *)DSPOp04,
    (void *)DSPOp08,
    (void *)DSPOp18,
    (void *)DSPOp28,
    (void *)DSPOp0C,
    (void *)DSPOp1C,
    (void *)DSPOp02,
    (void *)DSPOp06,
    (void *)DSPOp0E,
    (void *)DSPOp01,
    (void *)DSPOp11,
    (void *)DSPOp21,
    (void *)DSPOp0D,
    (void *)DSPOp0F,
    (void *)DSPOp1D,
    (void *)DSPOp2D,
    (void *)DSPOp03,
    (void *)DSPOp13,
    (void *)DSPOp23,
    (void *)DSPOp0B,
    (void *)DSPOp1B,
    (void *)DSPOp2B,
    (void *)DSPOp14,
    (void *)SPC7_Convert_Upper,
    (void *)SPC7_Convert_Lower,
    (void *)ST010DoCommand,
    (void *)RestoreCVFrame,
    (void *)BackupCVFrame,
    (void *)SRAMChdir,
    (void *)headerhack,
    (void *)C4TransfWireFrame2,
    (void *)C4CalcWireFrame,
    (void *)C4TransfWireFrame,
    (void *)C4Op15,
    (void *)C4Op1F,
    (void *)C4Op0D,
    (void *)SDD1_init,
    (void *)SDD1_get_byte,
    (void *)placedate,
    (void *)GetTime,
    (void *)clearmem2,
    (void *)DrawWater,
    (void *)DrawBurn,
    (void *)GetHostName,
    (void *)ZFileCHDir,
    (void *)clearmem,
    (void *)Setper2exec,
    (void *)clearSPCRAM,
    (void *)IntlEHi,
    (void *)pushdir,
    (void *)PatchUsingIPS,
    (void *)popdir,
    (void *)SPC7PackIndexLoad,
    (void *)loadROM,
    (void *)CalcChecksum,
    (void *)DumpROMLoadInfo,
    (void *)SetupSramSize,
    (void *)BankCheck,
    (void *)MirrorROM,
    (void *)chip_detect,
    (void *)InitDSP,
    (void *)ZFileSystemInit,
    (void *)obtaindir,
    (void *)getcfg,
    (void *)ConvertJoyMap,
    (void *)ccmdline,
    (void *)DOScreatenewcfg,
    (void *)zl_putchar,
    (void *)zl_getchar,
    (void *)ZOpenFile,
    (void *)ZFileSeek,
    (void *)ZFileWrite,
    (void *)ZFileRead,
    (void *)ZFileDelete,
    (void *)ZCloseFile,
    (void *)ZFileTell,
    (void *)GetDate,
    (void *)ZFileGetFTime,
    (void *)ZFileMKDir,
    (void *)ZFileRMDir,
    (void *)ZFileGetDir,
    (void *)ZFileFindFirst,
    (void *)ZFileFindNext,
    (void *)Start60HZ,
    (void *)initwinvideo,
    (void *)clearwin,
    (void *)Stop60HZ,
    (void *)drawscreenwin,
    (void *)UpdateVFrame,
    (void *)GetMouseX,
    (void *)GetMouseY,
    (void *)GetMouseButton,
    (void *)GetMouseMoveX,
    (void *)GetMouseMoveY,
    (void *)Start36HZ,
    (void *)Stop36HZ,
    (void *)CheckTimers,
    (void *)sem_sleep,
    (void *)InitTCP,
    (void *)StartServerCycle,
    (void *)ServerCheckNewClient,
    (void *)acceptzuser,
    (void *)ConnectServer,
    (void *)WaitForServer,
    (void *)SendData,
    (void *)SendDataUDP,
    (void *)GetData,
    (void *)DeInitTCP,
    (void *)StopServer,
    (void *)Disconnect,
    (void *)UDPClearVars,
    (void *)UDPWait1Sec,
    (void *)UDPEnableMode,
    (void *)UDPDisableMode,
    (void *)WinErrorA2,
    (void *)WinErrorB2,
    (void *)WinErrorC2,
    (void *)ZsnesPage,
    (void *)GetLocalTime,
    (void *)zl_malloc,
    (void *)GetFilename,
    (void *)LinuxExit,
    (void *)cycleinputdevice,
    0 };
