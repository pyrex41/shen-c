To install under SBCL / Windows 
________________________________
1. Start SBCL inside this home directory.

2. Enter (load "install.lsp")
   
3. Enter y/n to the requests thrown up by the installation process.  
   If you choose to install the Shen IDE you will nominate the current
   directory.

4. The executable `sbcl-shen.exe` will be created in the current directory.

Running Shen with Tk
___________________

1.  Click on sbcl-shen.exe in your home directory to get the REPL.
2.  Click on root.tcl in your nominated directory.
3.  Enter (ide.myIDE) to the REPL.

The IDE (root window) will appear.  If you want to kill 
the IDE, click the Exit button in the IDE and confirm that 
you wish to exit *before* closing the command window or the root window.

This ensures a clean shutdown.  Failure to do this will leave 
a zombie Tcl process running, which will interfere with subsequent 
sessions. In that case, you must terminate the zombie TCL process using 
the Task Manager.

Compiling the shen Kernel
_________________________

During installation you will be asked whether to create an image
*with* or *without* the Standard Library.

- Choose *with* unless you intend to build an image to compile
  the Shen kernel itself.
- Choose *without* only for kernel development.



