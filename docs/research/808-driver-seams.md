# Nokia 808 (RM-807, 113.010.1508): driver seam catalogue

Date: 2026-11-19. Device `nokia-808-rm807`, firmware key `RM-807/113.010.1508/04.01+C00.01`. Serves
docs/research/808-registry-review-2026-11-17.md section 4 ("keep the phone's LDD, write a PDD for a QEMU device").
Machine-readable copy: `build/registry/808-driver-seams.json` (same columns, one object per row).

Marks: **[V]** read from the files (imports.json, core.json, hybrid-manifest.txt, a strings scan of the
extracted XIP and ROFS binaries), **[I]** inferred. No decompilation was used.

## How the rows were found

- Modules: every E32 image in `imports.json` that imports `ekern.exe`, plus every `.ldd/.pdd/.drv/.ext` [V].
  That is 128 modules: 39 LDD, 11 PDD, 75 kernel extensions/kernel-side DLLs
  (one `.ext`), 3 user-side `.drv` (esock packet drivers on ROFS1, no kernel imports: listed for completeness).
  The 50 `.ldd/.pdd` (39 + 11) are exactly the 50 lines of `kernel-drivers.txt` [V]; the registry note says 49.
- Layer: all kernel modules are XIP, in the firmware's kernel-side image (`core.json` files below
  0x804b2b80, `kernel-side.img`). Our hybrid ROM keeps the firmware's user-side XIP from 0x804b2b80
  (1703 files, column `N95` in `hybrid-manifest.txt`) and replaces the whole kernel-side image with 39
  `OURS` files: our kernel, our EPL baseport modules and the stand-ins [V]. So **every firmware kernel
  module is out of our ROM**, whether `missing_drivers` names it or not: 9 of the 50 `.ldd/.pdd`
  (NfcDrv, cec_access, diproldd/dipropdd, eusbcsc, me_drv.ldd/.pdd, nfe_med_ext, ostbufferwriter, sgdevice) are
  omitted without being declared ("undeclared omission" in the notes) [V].
- `missing_drivers` also names 4 modules this firmware does not contain (tracecoreostldd.ldd, d_ftrace.ldd,
  proximityldd.ldd, proximitypdd.pdd: stale entries, the 808's proximity driver is diproldd/dipropdd) and 3
  whose EPL build we ship under the same name (ecomm, elocd, pipelib) [V].
- Imports: deduplicated DLL names from `imports.json` [V]. "platform:" marks the platform-driver class
  (egpio, dma, i2c, spi, karap, gape, PowerResourceManager, power, usb*, tracecore...) that a PDD cannot
  carry onto our board; VideoCore kexts (vchi, vcdriver, ...) are the other blocking class.
- Partner: the `Ldd.Pdd`-form ASCII string in the PDD (e.g. `BC02.Rap`, `Media.Nand`, `wlan.phys`) [V],
  else the name convention [I].
- Opener: user binaries (XIP + ROFS1/2) whose UTF-16 or ASCII strings contain the driver's file name or
  the LDD's own name string (the string `RBusLogicalChannel::DoCreate` gets) [V]. "unknown" = no
  distinctive hit.
- Status: from `hybrid-manifest.txt`, the profile's `standins` and `missing_drivers` [V].

## Verdict counts

- defer: 47
- below the seam / not needed / omit: 41
- stand-in / replacement stays: 14
- needs our PDD/LDD (+ device model): 8
- runs: our EPL build of the same module: 8
- runs as-is on our kernel (over our PDD where one exists): 7
- not a kernel seam (user side): 3

## Logical device drivers (LDD)

| module | layer | kind | imports | partner | user-side opener | status today | verdict |
|---|---|---|---|---|---|---|---|
| aamdaifdriver.ldd | XIP (kernel-side image) | LDD | ekern + platform: powerresourcemanager.dll | none (no PDD; DSP shared memory via ekern) [V] | aamdaifapi.dll (name 'MdaifDriver') [V] | replaced by stand-in aamdaifdriver | defer: LDD imports only ekern + PowerResourceManager, but it drives the RAPIDO DSP ring in shared memory; needs a RAPIDO DSP device model + a PRM stub [I] |
| accelerometer.ldd | XIP (kernel-side image) | LDD | ekern only | accelerometer.pdd (LDD name 'Accelerometer' in the PDD) [V] | accssy.dll [V] | replaced by stand-in accelerometer | runs as-is on our kernel over our PDD [I: ekern-only imports; ABI check pending] |
| audiopipeldd.ldd | XIP (kernel-side image) | LDD | ekern + vcdriver.dll, vchi.dll | none (VideoCore client) [V] | hdmiaudiodata.dll [V] | omitted (missing_drivers) | defer: VideoCore (vchi) audio pipe for HDMI [V imports] |
| BC02.LDD | XIP (kernel-side image) | LDD | ekern only | bc02_uart1.PDD (PDD name 'BC02.Rap') [V] | h4plushciv2_hctl.dll (name 'BC02') [V] | omitted (missing_drivers) | runs as-is on our kernel over our PDD [I: ekern-only; docs/research/hctl-bc02-808.md lists the kernel-internal APIs to check] |
| camldd.ldd | XIP (kernel-side image) | LDD | ekern + dcam_policy.dll, dcam_use_case.dll | none (imports dcam_policy/dcam_use_case: VideoCore camera stack) [V] | rcam.dll (name 'camdrv') [V] | omitted (missing_drivers) | defer: camera lives on the VideoCore [V imports] |
| cec_access.ldd | XIP (kernel-side image) | LDD | ekern + display_chipset_api_interface_storage.dll | none (display_chipset_api_interface_storage) [V] | cec_access.dll (name 'CEC_LDD') [V] | omitted (kernel-side image not used; not declared) | defer: HDMI CEC through the display chipset; nothing opens it without HDMI [I] |
| custom.ldd | XIP (kernel-side image) | LDD | ekern + platform: gape.dll, karap.dll; isaaccessextension.dll, isimessagekernel.dll | none [V] | sastartupservice.dll / nokiatsy.dll (string 'custom'; many hits, opener uncertain) [I] | replaced by stand-in custom | stand-in stays: the LDD itself imports gape, isaaccessextension, isimessagekernel, karap (no PDD split) [V] |
| d_display.ldd | XIP (kernel-side image) | LDD | ekern + platform: egpio.dll, powerresourcemanager.dll; display_chipset_api_interface_storage.dll, display_modules.dll, display_tools.dll, tv_out.dll | none (display_* kexts, tv_out, egpio, PRM) [V] | r_display.dll (name 'DisplayDriver') [V] | replaced by stand-in d_display | stand-in stays: the LDD drives the display chipset directly (no PDD split) [V] |
| ddc_accessldd.ldd | XIP (kernel-side image) | LDD | ekern + platform: powerresourcemanager.dll; dispcommanddrv.dll | none (dispcommanddrv -> vchi) [V] | ddc_access.dll [V] | omitted (missing_drivers) | defer: HDMI DDC via VideoCore [V imports] |
| diproldd.ldd | XIP (kernel-side image) | LDD | ekern only | dipropdd.pdd (name convention) [I] | prxssy.dll (name 'dipro') [V] | omitted (kernel-side image not used; not declared) | runs as-is on our kernel over our PDD [I: ekern-only]; proximity sensor |
| dvfs_interface.ldd | XIP (kernel-side image) | LDD | ekern + platform: karap.dll, powerresourcemanager.dll | none (karap, PRM) [V] | unknown (name 'DVFSInterfaceDriver'; no user string hit) [V] | omitted (missing_drivers) | omit: test/debug interface to DVFS; nothing found that opens it [I] |
| ecomm.ldd | XIP (kernel-side image) | LDD | ekern only | Comm PDDs (EPL) [V] | c32.dll [V] | our EPL build in the ROM (same name) [also listed in missing_drivers] | runs: our EPL build of the same module is in the ROM (OURS) [V] |
| elocd.ldd | XIP (kernel-side image) | LDD | ekern only | Media.* PDDs [V] | efile.exe/file server (name 'LocDrv') [I] | our EPL build in the ROM (same name) [also listed in missing_drivers] | runs: our EPL build is in the ROM (OURS) [V] |
| eusbc.ldd | XIP (kernel-side image) | LDD | ekern + platform: usbcc.dll | none (imports usbcc = USB client controller kext) [V] | ecacm.csy, msfs.fsy, usb_charging_service.dll, obexusbtransportcontroller.dll, whcmclasscontroller.dll (name 'Usbc') [V] | omitted (missing_drivers) | defer: needs a USB device-controller model behind usbcc [V imports] |
| eusbcsc.ldd | XIP (kernel-side image) | LDD | ekern + platform: usbcc.dll | none (usbcc) [V] | msfs.fsy, ncmclasscontroller.dll (name 'Usbcsc') [V] | omitted (kernel-side image not used; not declared) | defer: shared-chunk USB client, same controller [V] |
| isaaccessldd.ldd | XIP (kernel-side image) | LDD | ekern + isaaccessextension.dll | none (isaaccessextension) [V] | iscapi.dll (name 'IADDriver') [V] | replaced by stand-in isaaccessldd | replaced by stand-in isaaccessldd (fake modem); stays [V] |
| khbridge.ldd | XIP (kernel-side image) | LDD | ekern + vcdriver.dll, vchi.dll | none (vchi) [V] | unknown (strings 'KHRN', 'KHRSendCtrl'; vcbackendserver.dll likely) [I] | omitted (missing_drivers) | defer: VideoCore KHRN bridge [V imports] |
| khegl_ldd.ldd | XIP (kernel-side image) | LDD | ekern + vchi.dll, virtualsurface.dll | none (vchi, virtualsurface) [V] | vcbackendserver.dll [V] | omitted (missing_drivers) | defer: VideoCore EGL [V] |
| khmonitor.ldd | XIP (kernel-side image) | LDD | ekern + khbridge.ldd | none (khbridge) [V] | unknown (no strings) [V] | omitted (missing_drivers) | defer: VideoCore [V] |
| lightldd.ldd | XIP (kernel-side image) | LDD | ekern + platform: gape.dll | lightpdd.pdd [V] | salightsservice.dll (name 'LightDrv') [V] | replaced by stand-in lightldd | replaced by stand-in lightldd; the firmware LDD imports gape, so no as-is run [V] |
| magnetometerldd.ldd | XIP (kernel-side image) | LDD | ekern only | magnetometerpdd.pdd (name convention) [I] | magnetometer_ssy.dll (name 'magnetometer') [V] | omitted (missing_drivers) | runs as-is on our kernel over our PDD [I: ekern-only] |
| me_drv.ldd | XIP (kernel-side image) | LDD | ekern only | me_drv.pdd (PDD string 'Rapu_Cost5GpsDriver': GPS) [I] | unknown (no user string hit for 'me_drv') [V] | omitted (kernel-side image not used; not declared) | runs as-is over our PDD [I: ekern-only]; GPS receiver; opener unknown, so defer |
| mobilecrashdriver.ldd | XIP (kernel-side image) | LDD | ekern only | none [V] | unknown (strings 'MINKDA', 'KERN-EXEC'; a stand-in dir src/standins/mobilecrashdriver exists) [I] | omitted (missing_drivers) | could run as-is [I: ekern-only, uses kernel debug agent hooks]; omit |
| NfcDrv.LDD | XIP (kernel-side image) | LDD | ekern + platform: egpio.dll, i2c0.dll, powerresourcemanager.dll | none: the LDD drives the NFC chip itself (egpio, i2c0, PRM) [V] | unknown ('nfc' too common: 80 hits) [V] | omitted (kernel-side image not used; not declared) | defer: must be rewritten whole over an NFC chip model; no PDD seam [V] |
| ostbufferwriter.ldd | XIP (kernel-side image) | LDD | ekern + platform: tracecore.dll | none (tracecore) [V] | ostbuffer.dll / tracebuffer.dll (name 'RTraceBuffer') [V] | omitted (kernel-side image not used; not declared) | omit: OST tracing [V] |
| otgdi.ldd | XIP (kernel-side image) | LDD | ekern + platform: usbhost.dll | none (usbhost) [V] | unknown (no strings) [V] | omitted (missing_drivers) | defer: USB OTG host [V imports] |
| pipelib.ldd | XIP | LDD | ekern only | none [V] | rpipe.dll [V] | our EPL build in the ROM (same name) [also listed in missing_drivers] | runs: our EPL build (OURS) [V] |
| rebootdriver.ldd | XIP (kernel-side image) | LDD | ekern + platform: karap.dll | none (karap) [V] | sastartupservice.dll (name 'rebootdriver') [V] | omitted (missing_drivers) | needs our LDD (tiny: Kern::Restart); cheap [I] |
| rm_debug.ldd | XIP (kernel-side image) | LDD | ekern only | none [V] | unknown (TRK/CODA debug agent) [I] | our EPL build in the ROM (same name) | runs: our EPL build (OURS) [V] |
| secldd.ldd | XIP (kernel-side image) | LDD | ekern + platform: powerresourcemanager.dll; dataserviceapi.dll, isaaccessextension.dll, isimessagekernel.dll, r_display_kernel.dll | none (dataserviceapi, isa, PRM, r_display_kernel) [V] | secenv.dll, nfe_sec_drv_proxy.dll (name 'SecEnv') [V] | replaced by stand-in secldd | stand-in stays: no PDD split, VideoCore + ISI inside [V] |
| sgdevice.ldd | XIP (kernel-side image) | LDD | ekern + sgextension.dll | none (sgextension -> khbridge) [V] | sgresource.dll [V] | omitted (kernel-side image not used; not declared) | defer: VideoCore graphics resources [V imports] |
| surfacemanagerdriver.ldd | XIP (kernel-side image) | LDD | ekern + dvcsurface.dll | none (dvcsurface) [V] | surfacemanager.dll [V] | replaced by stand-in surfacemanagerdriver | replaced by stand-in surfacemanagerdriver (EPL surface manager) [V] |
| tcldd.ldd | XIP (kernel-side image) | LDD | ekern + platform: tracecore.dll | none (tracecore) [V] | unknown (name 'TcLdd') [V] | omitted (missing_drivers) | omit: trace core [V] |
| usbaudiodriver.ldd | XIP (kernel-side image) | LDD | ekern + platform: gape.dll; isaaccessextension.dll, isimessagekernel.dll | none (gape, isa) [V] | usbaudiodriverproxy.dll (name 'USBAudioDrv') [V] | replaced by stand-in usbaudiodriver | replaced by stand-in usbaudiodriver (requests pend) [V] |
| usbdi.ldd | XIP (kernel-side image) | LDD | ekern + platform: usbhost.dll | none (usbhost) [V] | usbdi_utils.dll, usbaudioadaptationplugin.dll (name 'USBDI') [V] | omitted (missing_drivers) | defer: USB host [V] |
| usbhubdriver.ldd | XIP (kernel-side image) | LDD | ekern + platform: usbhost.dll | none (usbhost) [V] | unknown (string 'USBHubDriver' is in usbdi.ldd) [V] | omitted (missing_drivers) | defer: USB host [V] |
| vcdispmanldd.ldd | XIP (kernel-side image) | LDD | ekern + dispcommanddrv.dll, tv_out.dll, vcbufmanx.dll, vcdispmanupdatehandler.dll, vcdispmanvchi.dll | none (dispcommanddrv, tv_out, vcbufmanx, vcdispman*) [V] | compositionbackend.dll, vcbackendserver.dll [V] | omitted (missing_drivers) | replaced by the gcebe compositor (composition.impl); defer the original [V] |
| vcfilesys.ldd | XIP (kernel-side image) | LDD | ekern + vcdriver.dll, vchi.dll | none (vchi) [V] | unknown (strings 'FCON', 'FSYS') [V] | omitted (missing_drivers) | defer: VideoCore file system service [V] |
| wlanldd.ldd | XIP (kernel-side image) | LDD | ekern only | wlanpdd.pdd (PDD name 'wlan.phys') [V] | wlmserversrv.dll [V] | omitted (missing_drivers) | runs as-is over our PDD only in theory [I: ekern-only, 165 KB 802.11 MAC in the LDD]; defer: the PDD is a 395 KB chip driver over SPI |

## Physical device drivers (PDD)

| module | layer | kind | imports | partner | user-side opener | status today | verdict |
|---|---|---|---|---|---|---|---|
| accelerometer.pdd | XIP (kernel-side image) | PDD | ekern + platform: egpio.dll, gape.dll, i2c0.dll, powerresourcemanager.dll | accelerometer.ldd [V] | accssy.dll (loads the .pdd by file name) [V] | replaced by stand-in accelerometerpdd | needs our PDD on syborg-sensor (imports egpio, gape, i2c0, PRM) [V] |
| bc02_uart1.PDD | XIP (kernel-side image) | PDD | ekern + platform: dma.dll, egpio.dll, karap.dll, powerresourcemanager.dll | BC02.LDD [V] | h4plushciv2_hctl.dll (file name 'BC02_UART1') [V] | omitted (missing_drivers) | needs our PDD on syborg-serial + a minimal HCI controller (imports dma, egpio, karap, PRM) [V] |
| dipropdd.pdd | XIP (kernel-side image) | PDD | ekern + platform: egpio.dll, i2c0.dll, powerresourcemanager.dll | diproldd.ldd (name convention) [I] | prxssy.dll (loads the .pdd by file name) [V] | omitted (kernel-side image not used; not declared) | needs our PDD on syborg-sensor (imports egpio, i2c0, PRM) [V] |
| lightpdd.pdd | XIP (kernel-side image) | PDD | ekern + platform: gape.dll; r_display_kernel.dll | lightldd.ldd [V] | salightsservice.dll [V] | replaced by stand-in lightpdd | replaced by stand-in lightpdd (load-only) [V] |
| magnetometerpdd.pdd | XIP (kernel-side image) | PDD | ekern + platform: egpio.dll, i2c0.dll, powerresourcemanager.dll | magnetometerldd.ldd [I] | magnetometer_ssy.dll, tiltcompensationssy.dll [V] | omitted (missing_drivers) | needs our PDD on syborg-sensor (egpio, i2c0, PRM) [V] |
| me_drv.pdd | XIP (kernel-side image) | PDD | ekern + platform: egpio.dll, i2c1.dll, powerresourcemanager.dll | me_drv.ldd [I] | unknown | omitted (kernel-side image not used; not declared) | needs our PDD on a GPS device model (egpio, i2c1, PRM) [V imports]; defer until the opener is known |
| medint.pdd | XIP (kernel-side image) | PDD | ekern + elocd.ldd | elocd.ldd (PDD name 'Media.IRam') [V] | n/a (local media) | our EPL build in the ROM (same name) [also listed in missing_drivers] | runs: our EPL build (OURS) [V] |
| mednand.pdd | XIP (kernel-side image) | PDD | ekern + platform: dma.dll, egpio.dll, gape.dll, powerresourcemanager.dll; elocd.ldd | elocd.ldd ('Media.Nand') [V] | n/a | omitted (missing_drivers) | replaced by medrofs/mednvmem2 stand-ins (flash window, E:) [V] |
| MEDSD.PDD | XIP (kernel-side image) | PDD | ekern + elocd.ldd, emmcptn.dll, epbusm.dll | elocd.ldd ('Media.MmcF'; emmcptn, epbusm) [V] | n/a | omitted (missing_drivers) | needs our PDD on an SD/MMC model for the memory card (F:) [I]; cheap with the EPL medsd |
| nfe_med_ext.pdd | XIP (kernel-side image) | PDD | ekern + elocd.ldd, nfe_enc_proxy.dll | elocd.ldd ('Media.NFE') [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: encrypted-media extension over mednand [I] |
| wlanpdd.pdd | XIP (kernel-side image) | PDD | ekern + platform: egpio.dll, spi2.dll, wlanhpa.dll | wlanldd.ldd [V] | wlmserversrv.dll [V] | omitted (missing_drivers) | defer: needs a WLAN chip model over spi2 + wlanhpa [V imports] |

## Kernel extensions and kernel-side DLLs

| module | layer | kind | imports | partner | user-side opener | status today | verdict |
|---|---|---|---|---|---|---|---|
| atmel_qt.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, gape.dll, i2c0.dll, powerresourcemanager.dll; isaaccessextension.dll, isimessagekernel.dll | none (touch controller kext: Atmel QT, I2C + ISI) [I] | n/a (kext) | omitted (kernel-side image not used; not declared) | replaced: exyin stand-in owns the pointer; not needed [V] |
| dataserviceapi.dll | XIP (kernel-side image) | kext | ekern + platform: powerresourcemanager.dll, securitykext.dll; vchi.dll | kext (vchi + securitykext) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore data service used by secldd [V imports] |
| dcam_policy.dll | XIP (kernel-side image) | kext | ekern + camldd.ldd, dcam_use_case.dll, dcamdecoder.dll, dcamencode.dll, dcamera.dll, dcamtest.dll, dvcrmiservice.dll, vcdriver.dll, vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcam_shared_vid.dll | XIP (kernel-side image) | kext | ekern + dcam_use_case.dll, dstreamiouser.dll, vchi.dll, vcstreamio.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcam_use_case.dll | XIP (kernel-side image) | kext | ekern + vcbufmanx.dll, vcdriver.dll, vchi.dll, virtualsurface.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcamdecoder.dll | XIP (kernel-side image) | kext | ekern + dcam_use_case.dll, dstreamiouser.dll, vchi.dll, vcstreamio.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcamencode.dll | XIP (kernel-side image) | kext | ekern + dcam_shared_vid.dll, dcam_use_case.dll, vcbufmanx.dll, vchi.dll, virtualsurface.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcamera.dll | XIP (kernel-side image) | kext | ekern + platform: powerresourcemanager.dll; dcam_shared_vid.dll, dcam_use_case.dll, dstreamiouser.dll, dvcrmiservice.dll, mkncodec.dll, vchi.dll, vcstreamio.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dcamtest.dll | XIP (kernel-side image) | kext | ekern + dcam_use_case.dll, vcdriver.dll, vchi.dll, vcstreamio.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore camera [V] |
| dispcommanddrv.dll | XIP (kernel-side image) | kext | ekern + vcdriver.dll, vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore display command path [V] |
| display_chipset_api_interface_storage.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only registry [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: d_display stand-in [V] |
| display_chipset_drv_ive_bc.dll | XIP (kernel-side image) | kext | ekern + dispcommanddrv.dll, display_chipset_api_interface_storage.dll, vc_logging.dll, vcbufmanx.dll, vcdispman.dll, vcdriver.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore display chipset driver [V] |
| display_tools.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: d_display stand-in [V] |
| dma.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: OMAP DMA, replaced by our kernel/baseport [V] |
| dstreamiouser.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore stream I/O [I] |
| dvcrmiservice.dll | XIP (kernel-side image) | kext | ekern + vcdriver.dll, vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| dvcsurface.dll | XIP (kernel-side image) | kext | ekern + vcdriver.dll, vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore surfaces (surfacemanagerdriver stand-in covers the user side) [V] |
| dvfs.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll, power.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: voltage/frequency scaling [V] |
| ecust.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll, karap.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: customisation/variant data [V] |
| egpio.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: OMAP GPIO [V] |
| EMMCPTN.DLL | XIP (kernel-side image) | kext | ekern + elocd.ldd, epbusm.dll | kext (eMMC partition, with epbusm) [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: medrofs/mednvmem2 stand-ins serve the drives [V] |
| empmIsaIf.dll | XIP (kernel-side image) | kext | ekern + platform: powerresourcemanager.dll, sisa_wrapper.dll; isaaccessextension.dll, isimessagekernel.dll | platform kext (power management over ISI) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| EPBUSM.DLL | XIP (kernel-side image) | kext | ekern + elocd.ldd | kext (peripheral bus, EPL class) [V] | n/a | omitted (kernel-side image not used; not declared) | not needed today; our EPL epbusm would come with an SD model [I] |
| EPBUSMV.DLL | XIP (kernel-side image) | kext | ekern + platform: dma.dll, egpio.dll, gape.dll, karap.dll, powerresourcemanager.dll; epbusm.dll | kext (peripheral bus variant: dma, egpio, karap, PRM) [V] | n/a | omitted (kernel-side image not used; not declared) | needs replacement if an SD model is added [V] |
| evfp.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only (VFP support) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: our kernel [V] |
| exmoncommon.dll | XIP (kernel-side image) | kext | ekern only | kext [V] | n/a | our EPL build in the ROM (same name) | runs: our EPL build (OURS) [V] |
| exmondebug.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll; exmoncommon.dll | kext (gape) [V] | n/a | our EPL build in the ROM (same name) | runs: our EPL build (OURS) [V] |
| exstart.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | our EPL build in the ROM (same name) | runs: our EPL build (OURS) [V] |
| exyin.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll | kext (gape) [V] | n/a (digitiser extension) | replaced by stand-in exyin | replaced by stand-in exyin on syborg-pointer [V] |
| GAPE.DLL | XIP (kernel-side image) | kext | ekern only | kext, ekern-only (variant registry) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam; could run as-is [I, accelerometer-hw-path.md] |
| i2c0.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: OMAP I2C [V] |
| i2c1.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: OMAP I2C [V] |
| isaaccessextension.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | replaced: the isaaccessldd stand-in carries the fake modem [V] |
| isimessagekernel.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | replaced with the modem path [V] |
| karap.dll | XIP (kernel-side image) | kext | ekern only | platform kext, ekern-only (RAP/OMAP register access) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: our kernel [V] |
| nfe_enc_proxy.dll | XIP (kernel-side image) | kext | ekern + platform: securitykext.dll | kext (securitykext) [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: NAND flash encryption [I] |
| power.dll | XIP (kernel-side image) | kext | ekern + platform: empmisaif.dll, karap.dll, power_resources.dll, powerresourcemanager.dll, securitykext.dll, sisa_wrapper.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: power controller [V] |
| power_resources.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, karap.dll, powerresourcemanager.dll, sisa_wrapper.dll | platform kext (PRM PSL) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| PowerResourceManager.dll | XIP (kernel-side image) | kext | ekern only | platform kext, ekern-only (PRM PIL) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam; a PRM stub would let PRM-only LDDs bind [I] |
| r_display_kernel.dll | XIP (kernel-side image) | kext | ekern + d_display.ldd | kext (d_display) [V] | n/a | omitted (kernel-side image not used; not declared) | not needed: d_display stand-in [V] |
| securitykext.dll | XIP (kernel-side image) | kext | ekern + platform: powerresourcemanager.dll; isaaccessextension.dll, isimessagekernel.dll | platform kext (ISI) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| sgextension.dll | XIP (kernel-side image) | kext | ekern + khbridge.ldd | kext (khbridge) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| signed_2763c1_v1.dll | XIP (kernel-side image) | kext | ekern + vc_exe_registry.dll | kext (vc_exe_registry: VideoCore firmware blob) [I] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| sisa_wrapper.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, empmisaif.dll, karap.dll, power.dll; isaaccessextension.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| spi1.dll | XIP (kernel-side image) | kext | ekern + platform: dma.dll, karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: OMAP SPI [V] |
| spi2.dll | XIP (kernel-side image) | kext | ekern + platform: dma.dll, karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam (WLAN bus) [V] |
| spi3.dll | XIP (kernel-side image) | kext | ekern + platform: dma.dll, karap.dll, powerresourcemanager.dll | platform kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| ssc.ext | XIP (kernel-side image) | kext (.ext) | ekern + platform: power.dll; isaaccessextension.dll, isimessagekernel.dll | kext (.ext: isa, isimessagekernel, power) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: ISI-side power/startup [I] |
| switch.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, gape.dll | kext (egpio, gape) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: switches/slides [I] |
| thermaleventhandler.dll | XIP (kernel-side image) | kext | ekern + platform: empmisaif.dll; r_display_kernel.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: thermal [V] |
| thermalmanager.dll | XIP (kernel-side image) | kext | ekern + platform: empmisaif.dll; thermalsensor.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| thermalsensinghwadaptation.dll | XIP (kernel-side image) | kext | ekern + platform: empmisaif.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| thermalsensor.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll; thermalsensinghwadaptation.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam [V] |
| tracecore.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | omit: tracing [V] |
| tracecoreconfigurationplugin.dll | XIP (kernel-side image) | kext | ekern + platform: tracecore.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | omit [V] |
| tracecoreisaifplugin.dll | XIP (kernel-side image) | kext | ekern + platform: tracecore.dll; isaaccessextension.dll, isimessagekernel.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | omit [V] |
| tv_out.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, powerresourcemanager.dll, securitykext.dll; vcdriver.dll, vchi.dll | kext (vchi, egpio, PRM) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: TV-out via VideoCore [V] |
| usb.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, gape.dll, karap.dll, powerresourcemanager.dll; isaaccessextension.dll, isimessagekernel.dll | platform kext (egpio, gape, isa, karap, PRM) [V] | n/a | omitted (kernel-side image not used; not declared) | below the seam: USB PHY/transceiver [I] |
| usbcc.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll, usb.dll | kext (USB client controller: gape, usb) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: needs a USB device-controller model [V] |
| usbhost.dll | XIP (kernel-side image) | kext | ekern + platform: gape.dll, usb.dll, usbcc.dll | kext (gape, usb, usbcc) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: USB host controller [V] |
| vc_exe_registry.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vc_logging.dll | XIP (kernel-side image) | kext | ekern + vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vcbufmanx.dll | XIP (kernel-side image) | kext | ekern + vcdriver.dll, vchi.dll, vcos.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vcdispman.dll | XIP (kernel-side image) | kext | ekern + vcdispmanupdatehandler.dll, vcdispmanvchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vcdispmanupdatehandler.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vcdispmanvchi.dll | XIP (kernel-side image) | kext | ekern + vcdispmanupdatehandler.dll, vchi.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| vcdriver.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, karap.dll, powerresourcemanager.dll; vc_exe_registry.dll | kext (egpio, karap, PRM, vc_exe_registry): the VideoCore host driver [V] | n/a | omitted (kernel-side image not used; not declared) | defer: BCM2763 [V] |
| vchi.dll | XIP (kernel-side image) | kext | ekern + vcdriver.dll | kext (vcdriver) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore host interface [V] |
| vcos.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore OS shim [V] |
| vcstreamio.dll | XIP (kernel-side image) | kext | ekern + vchi.dll, vcos.dll | kext [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| virtualsurface.dll | XIP (kernel-side image) | kext | ekern + dvcsurface.dll | kext (dvcsurface) [V] | n/a | omitted (kernel-side image not used; not declared) | defer: VideoCore [V] |
| wlanhpa.dll | XIP (kernel-side image) | kext | ekern + platform: egpio.dll, powerresourcemanager.dll | kext (egpio, PRM): WLAN host power adaptation [V] | n/a | omitted (kernel-side image not used; not declared) | defer: WLAN chip [V] |
| xtirx.dll | XIP (kernel-side image) | kext | ekern + platform: karap.dll, powerresourcemanager.dll, xtitx.dll | kext (karap, PRM) [V] | n/a | omitted (kernel-side image not used; not declared) | omit: XTI trace port [V] |
| xtitx.dll | XIP (kernel-side image) | kext | ekern only | kext, ekern-only [V] | n/a | omitted (kernel-side image not used; not declared) | omit: XTI trace port [V] |
| xtiwriter.dll | XIP (kernel-side image) | kext | ekern + platform: tracecore.dll, xtitx.dll | kext (tracecore, xtitx) [V] | n/a | omitted (kernel-side image not used; not declared) | omit [V] |

## User-side `.drv` (not kernel modules)

| module | layer | kind | imports | partner | user-side opener | status today | verdict |
|---|---|---|---|---|---|---|---|
| etherpkt.drv | ROFS1 | user .drv (esock packet driver) | ekern + drtaeabi.dll, efsrv.dll, esock.dll, ethint.nif, euser.dll, mbufmgr.dll, scppnwdl.dll | user-side esock packet driver (imports esock, euser) [V] | ethint.nif [V] | in our ROM as-is (ROFS, user side) | not a kernel seam: user side, runs as-is [V] |
| IrLAN.drv | ROFS1 | user .drv (esock packet driver) | ekern + drtaeabi.dll, esock.dll, esocksvr.dll, ethint.nif, euser.dll, irda.dll, mbufmgr.dll, scppnwdl.dll | user-side esock packet driver [V] | ethint.nif [V] | in our ROM as-is (ROFS, user side) | not a kernel seam: user side [V] |
| ncmpkt.drv | ROFS1 | user .drv (esock packet driver) | ekern + drtaeabi.dll, esock.dll, ethint.nif, euser.dll, mbufmgr.dll, random.dll, scppnwdl.dll, usbcsc_bil.dll | user-side esock packet driver (usbcsc_bil) [V] | ethint.nif [V] | in our ROM as-is (ROFS, user side) | not a kernel seam [V] |

## Replacement order (by the QEMU device each seam needs, cheapest first)

1. **No new device: rebootdriver.ldd** (imports karap only; opener sastartupservice.dll). A 30-line LDD of
   our own that calls the kernel restart; removes one `missing_drivers` entry and gives the power menu a
   path to reboot [I].
2. **syborg-sensor (exists): the three sensor pairs.** accelerometer.ldd, diproldd.ldd (proximity) and
   magnetometerldd.ldd import only ekern [V]; their PDDs import egpio + i2c0 + PRM [V]. One PDD skeleton
   over syborg-sensor (one vector per sensor) serves all three and retires the accelerometer stand-in pair.
   Openers: accssy.dll, prxssy.dll, magnetometer_ssy.dll [V]. Open: the kernel-ABI check of the LDDs
   against our kernel (docs/devices/nokia-n8-rm596/accelerometer-hw-path.md, registry `sensors-hw`),
   and whether dipro/magnetometer PDD names match the LDD names (name convention only [I]).
3. **syborg-serial + a minimal HCI controller: BC02.LDD over our bc02 PDD.** The LDD is an ECOMM-style
   DComm LDD importing only ekern [V]; the PDD must register as `BC02.Rap` [V] and is loaded as
   `BC02_UART1` by h4plushciv2_hctl.dll [V]. Contract and the kernel-internal APIs to check are in
   docs/research/hctl-bc02-808.md and registry `bt-controller-link`. Removes H-fmradio-none,
   H-bt-ondemand and 2 `missing_drivers` entries (plan section 4 item 1).
4. **An SD/MMC model: MEDSD.PDD (`Media.MmcF`)** over our EPL elocd. Cheapest done with the EPL medsd +
   epbusm on a Syborg MMC device rather than the firmware PDD (it imports emmcptn/epbusm/epbusmv, platform
   class) [V]. Gives the memory card (F:). mednand/nfe_med_ext stay replaced by medrofs/mednvmem2.
5. **A GPS device model: me_drv.ldd/.pdd** (PDD string `Rapu_Cost5GpsDriver` [V]; LDD ekern-only [V];
   PDD on i2c1). Blocked on the open question: which user module opens `me_drv` (no string hit) [V].
6. **A USB device controller: eusbc.ldd / eusbcsc.ldd** (ekern + usbcc) [V]. The seam is usbcc.dll
   (the controller kext), not a PDD; needs a USB DC model and our usbcc. Host side (usbdi, usbhubdriver,
   otgdi over usbhost) is a second, separate model. Defer.
7. **A WLAN chip over SPI: wlanldd/wlanpdd.** LDD ekern-only but 165 KB of MAC logic; PDD 395 KB chip
   driver (spi2, wlanhpa) [V]. Too big to mock as a PDD; defer (a user-side stub over syborg-net is cheaper).
8. **NFC chip: NfcDrv.LDD** drives i2c0/egpio itself, no PDD seam [V]; opener unknown. Defer.
9. **VideoCore (BCM2763): everything else** (khbridge, khegl, khmonitor, sgdevice, vcdispmanldd, vcfilesys,
   audiopipeldd, camldd + dcam*, cec_access, ddc_accessldd, tv_out, the vc*/dvc* kexts, and the LDDs that
   embed VideoCore clients: d_display, secldd). Strategy `defer` (registry `video-core`); the gcebe
   compositor, d_display, secldd and surfacemanagerdriver stand-ins stay.
10. **Stays replaced with no seam:** custom.ldd (gape/ISI/karap inside the LDD), isaaccessldd (the fake
    modem), lightldd/lightpdd (LDD imports gape), usbaudiodriver (gape/ISI), aamdaifdriver (PRM only, but
    the RAPIDO DSP ring: audio-dsp component), exyin (touch).

Open questions:
- Kernel ABI: "imports only ekern" is necessary, not sufficient (hctl-bc02-808.md: kernel-internal APIs,
  object layouts). One binding test per LDD (load it on our kernel with an empty PDD) settles it.
- Openers unknown: me_drv (GPS), NfcDrv, khbridge/khmonitor, usbhubdriver/otgdi, vcfilesys, tcldd,
  dvfs_interface, mobilecrashdriver, rm_debug.
- PRM: PowerResourceManager.dll is ekern-only [V]; a PRM stub kext would let PRM-only LDDs
  (aamdaifdriver) bind, but their PSL (power_resources.dll) is platform class [V].
- Profile hygiene: drop the 4 stale `missing_drivers` names, add the 9 undeclared omissions, and stop
  listing ecomm/elocd/pipelib (our builds are in the ROM) [V].
