; forwarders.asm — transparent export thunks for the dxgi.dll proxy.
;
; Each exported name is a naked `jmp [ptr]` to the matching entry point in the
; genuine system dxgi.dll. The pointer slots are filled at load time by
; Exports_Init (exports.cpp) via GetProcAddress. Because these are tail jumps,
; every register and stack argument passes through untouched — so even the
; undocumented exports with signatures we never declare forward perfectly.

.data
PUBLIC p_ApplyCompatResolutionQuirking
PUBLIC p_CompatString
PUBLIC p_CompatValue
PUBLIC p_DXGIDumpJournal
PUBLIC p_PIXBeginCapture
PUBLIC p_PIXEndCapture
PUBLIC p_PIXGetCaptureState
PUBLIC p_SetAppCompatStringPointer
PUBLIC p_UpdateHMDEmulationStatus
PUBLIC p_CreateDXGIFactory
PUBLIC p_CreateDXGIFactory1
PUBLIC p_CreateDXGIFactory2
PUBLIC p_DXGID3D10CreateDevice
PUBLIC p_DXGID3D10CreateLayeredDevice
PUBLIC p_DXGID3D10GetLayeredDeviceSize
PUBLIC p_DXGID3D10RegisterLayers
PUBLIC p_DXGIDeclareAdapterRemovalSupport
PUBLIC p_DXGIDisableVBlankVirtualization
PUBLIC p_DXGIGetDebugInterface1
PUBLIC p_DXGIReportAdapterConfiguration

p_ApplyCompatResolutionQuirking    QWORD 0
p_CompatString                     QWORD 0
p_CompatValue                      QWORD 0
p_DXGIDumpJournal                  QWORD 0
p_PIXBeginCapture                  QWORD 0
p_PIXEndCapture                    QWORD 0
p_PIXGetCaptureState               QWORD 0
p_SetAppCompatStringPointer        QWORD 0
p_UpdateHMDEmulationStatus         QWORD 0
p_CreateDXGIFactory                QWORD 0
p_CreateDXGIFactory1               QWORD 0
p_CreateDXGIFactory2               QWORD 0
p_DXGID3D10CreateDevice            QWORD 0
p_DXGID3D10CreateLayeredDevice     QWORD 0
p_DXGID3D10GetLayeredDeviceSize    QWORD 0
p_DXGID3D10RegisterLayers          QWORD 0
p_DXGIDeclareAdapterRemovalSupport QWORD 0
p_DXGIDisableVBlankVirtualization  QWORD 0
p_DXGIGetDebugInterface1           QWORD 0
p_DXGIReportAdapterConfiguration   QWORD 0

.code

JMPTHUNK MACRO fname
fname PROC
    jmp QWORD PTR [p_&fname]
fname ENDP
ENDM

JMPTHUNK ApplyCompatResolutionQuirking
JMPTHUNK CompatString
JMPTHUNK CompatValue
JMPTHUNK DXGIDumpJournal
JMPTHUNK PIXBeginCapture
JMPTHUNK PIXEndCapture
JMPTHUNK PIXGetCaptureState
JMPTHUNK SetAppCompatStringPointer
JMPTHUNK UpdateHMDEmulationStatus
JMPTHUNK CreateDXGIFactory
JMPTHUNK CreateDXGIFactory1
JMPTHUNK CreateDXGIFactory2
JMPTHUNK DXGID3D10CreateDevice
JMPTHUNK DXGID3D10CreateLayeredDevice
JMPTHUNK DXGID3D10GetLayeredDeviceSize
JMPTHUNK DXGID3D10RegisterLayers
JMPTHUNK DXGIDeclareAdapterRemovalSupport
JMPTHUNK DXGIDisableVBlankVirtualization
JMPTHUNK DXGIGetDebugInterface1
JMPTHUNK DXGIReportAdapterConfiguration

END
