"""Native path selection only; no upload or copying."""
import sys
import os


def choose_path():
    if os.name == 'nt':
        # Native Windows dialog, without a Tk/Tcl installation dependency.
        import ctypes as c
        from ctypes import wintypes as w
        class OPENFILENAMEW(c.Structure):
            _fields_=[('lStructSize',w.DWORD),('hwndOwner',w.HWND),('hInstance',w.HINSTANCE),
                ('lpstrFilter',w.LPCWSTR),('lpstrCustomFilter',w.LPWSTR),('nMaxCustFilter',w.DWORD),
                ('nFilterIndex',w.DWORD),('lpstrFile',w.LPWSTR),('nMaxFile',w.DWORD),
                ('lpstrFileTitle',w.LPWSTR),('nMaxFileTitle',w.DWORD),('lpstrInitialDir',w.LPCWSTR),
                ('lpstrTitle',w.LPCWSTR),('Flags',w.DWORD),('nFileOffset',w.WORD),('nFileExtension',w.WORD),
                ('lpstrDefExt',w.LPCWSTR),('lCustData',c.c_ssize_t),('lpfnHook',c.c_void_p),
                ('lpTemplateName',w.LPCWSTR),('pvReserved',c.c_void_p),('dwReserved',w.DWORD),('FlagsEx',w.DWORD)]
        dll=c.WinDLL('comdlg32',use_last_error=True)
        dll.GetOpenFileNameW.argtypes=[c.POINTER(OPENFILENAMEW)]
        dll.GetOpenFileNameW.restype=w.BOOL
        buf=c.create_unicode_buffer(32768)
        request=OPENFILENAMEW()
        request.lStructSize=c.sizeof(request)
        request.lpstrFilter='Graph dataset descriptor (*.json)\0*.json\0\0'
        request.lpstrFile=c.cast(buf,w.LPWSTR)
        request.nMaxFile=len(buf)
        request.lpstrTitle='选择图数据集描述文件'
        request.lpstrInitialDir=os.path.join(os.path.dirname(os.path.dirname(__file__)),'data','descriptors')
        request.Flags=0x80000|0x1000|0x800|0x8|0x2000000
        if dll.GetOpenFileNameW(c.byref(request)):
            return buf.value
        error=dll.CommDlgExtendedError()
        if error:
            raise OSError(f'Native file picker failed: {error}')
        return ''  # User cancelled, not a failure.
    import tkinter as tk
    from tkinter import filedialog
    root=tk.Tk();root.withdraw()
    try:
        return filedialog.askopenfilename(parent=root,title='Select dataset descriptor',filetypes=[('Graph dataset descriptor','*.json')])
    finally:
        root.destroy()


if __name__ == '__main__':
    sys.stdout.buffer.write(choose_path().encode('utf-8'))
