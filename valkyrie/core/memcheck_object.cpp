/* --------------------------------------------------------------------- 
 * Implementation of class Memcheck                  memcheck_object.cpp
 * Memcheck-specific options / flags / fns
 * ---------------------------------------------------------------------
 * This file is part of Valkyrie, a front-end for Valgrind
 * Copyright (c) 2000-2005, OpenWorks LLP <info@open-works.co.uk>
 * This program is released under the terms of the GNU GPL v.2
 * See the file LICENSE.GPL for the full license details.
 */

#include "memcheck_object.h"
#include "valkyrie_object.h"
#include "vk_config.h"
#include "html_urls.h"
#include "vk_messages.h"
#include "vk_option.h"         // PERROR* and friends 
#include "vk_utils.h"          // vk_assert, VK_DEBUG, etc.

#include <qapplication.h>
#include <qtimer.h>
#include <qurloperator.h>


/* class Memcheck ------------------------------------------------------ */
Memcheck::~Memcheck()
{
   if (m_vgproc) {
      m_vgproc->disconnect(); /* so no signal calling processDone() */
      if (m_vgproc->isRunning()) {
         m_vgproc->stop(); 
      }
      delete m_vgproc;
      m_vgproc = 0;
   }
   if (m_vgreader) {
      delete m_vgreader;
      m_vgreader = 0;
   }

   /* m_logpoller deleted by it's parent: 'this' */

   /* unsaved log... delete our temp file */
   if (!m_fileSaved && !m_saveFname.isEmpty())
      QDir().remove( m_saveFname );
}


Memcheck::Memcheck( int objId ) 
   : ToolObject( "Memcheck", "&Memcheck", Qt::SHIFT+Qt::Key_M, objId ) 
{
   /* init vars */
   m_fileSaved = true;
   m_vgproc    = 0;
   m_vgreader  = 0;
   m_logpoller = new VkLogPoller( this, "memcheck logpoller" );
   connect( m_logpoller, SIGNAL(logUpdated()),
            this,        SLOT(readVgLog()) );

	/* these opts should be kept in exactly the same order as valgrind
      outputs them, as it makes keeping up-to-date a lot easier. */
   addOpt( LEAK_CHECK,  VkOPTION::ARG_STRING, VkOPTION::WDG_COMBO, 
           "memcheck",  '\0',                 "leak-check",
           "<no|summary|full>",  "no|summary|full",  "full",
           "Search for memory leaks at exit:",
           "search for memory leaks at exit?",
           urlMemcheck::Leakcheck );
   addOpt( LEAK_RES,    VkOPTION::ARG_STRING, VkOPTION::WDG_COMBO, 
           "memcheck",  '\0',                 "leak-resolution",
           "<low|med|high>", "low|med|high", "low",
           "Degree of backtrace merging:",
           "how much backtrace merging in leak check", 
           urlMemcheck::Leakres );
   addOpt( SHOW_REACH,  VkOPTION::ARG_BOOL,   VkOPTION::WDG_CHECK, 
           "memcheck",  '\0',                 "show-reachable",
           "<yes|no>",  "yes|no",             "no",
           "Show reachable blocks in leak check",
           "show reachable blocks in leak check?",  
           urlMemcheck::Showreach );
   addOpt( UNDEF_VAL,   VkOPTION::ARG_BOOL,   VkOPTION::WDG_CHECK, 
           "memcheck",  '\0',                 "undef-value-errors",
           "<yes|no>",  "yes|no",             "yes",
           "Check for undefined value errors",
           "check for undefined value errors?",
           urlMemcheck::UndefVal );
   addOpt( PARTIAL,     VkOPTION::ARG_BOOL,   VkOPTION::WDG_CHECK, 
           "memcheck",  '\0',                 "partial-loads-ok",
           "<yes|no>",  "yes|no",             "no",
           "Ignore errors on partially invalid addresses",
           "too hard to explain here; see manual",
           urlMemcheck::Partial );
   addOpt( FREELIST,    VkOPTION::ARG_UINT,   VkOPTION::WDG_LEDIT, 
           "memcheck",  '\0',                 "freelist-vol",
           "<number>",  "0|10000000",         "5000000",
           "Volume of freed blocks queue:",
           "volume of freed blocks queue",
           urlMemcheck::Freelist );
   addOpt( GCC_296,     VkOPTION::ARG_BOOL,   VkOPTION::WDG_CHECK, 
           "memcheck",  '\0',                 "workaround-gcc296-bugs",
           "<yes|no>",  "yes|no",             "no",
           "Work around gcc-296 bugs",
           "self explanatory",  
           urlMemcheck::gcc296 );
   addOpt( ALIGNMENT,  VkOPTION::ARG_PWR2,   VkOPTION::WDG_SPINBOX, 
           "memcheck",  '\0',                "alignment", 
           "<number>", "8|1048576",          "8",
           "Minimum alignment of allocations",
           "set minimum alignment of allocations", 
           urlVgCore::Alignment );
}


/* check argval for this option, updating if necessary.
   called by parseCmdArgs() and gui option pages -------------------- */
int Memcheck::checkOptArg( int optid, QString& argval )
{
   vk_assert( optid >= 0 && optid < NUM_OPTS );

   int errval = PARSED_OK;
   Option* opt = findOption( optid );

   switch ( (Memcheck::mcOpts)optid ) {
   case PARTIAL:
   case FREELIST:
   case LEAK_RES:
   case SHOW_REACH:
   case UNDEF_VAL:
   case GCC_296:
   case ALIGNMENT:
      opt->isValidArg( &errval, argval );
      break;

      /* when using xml output from valgrind, this option is preset to
         'full' by valgrind, so this option should not be used. */
   case LEAK_CHECK:
      /* Note: gui options disabled, so only reaches here from cmdline */
      errval = PERROR_BADOPT;
      vkPrintErr("Option disabled '--%s'", opt->m_longFlag.latin1());
      vkPrintErr(" - Memcheck presets this option to 'full' when generating the required xml output.");
      vkPrintErr(" - See valgrind/docs/internals/xml_output.txt.");
      break;

   default:
      vk_assert_never_reached();
   }

   return errval;
}


/* called from Valkyrie::updateVgFlags() whenever flags have been changed */
QStringList Memcheck::modifiedVgFlags()
{
   QStringList modFlags;
   QString defVal, cfgVal, flag;

   Option* opt;
   for ( opt = m_optList.first(); opt; opt = m_optList.next() ) {
      flag   = opt->m_longFlag.isEmpty() ? opt->m_shortFlag
                                         : opt->m_longFlag;
      defVal = opt->m_defaultValue;     /* opt holds the default */
      cfgVal = vkConfig->rdEntry( opt->m_longFlag, name() );

      switch ( (Memcheck::mcOpts)opt->m_key ) {

      /* when using xml output from valgrind, this option is preset to
         'full' by valgrind, so this option should not be used. */
      case LEAK_CHECK:
         /* ignore this opt */
         break;

      default:
         if ( defVal != cfgVal )
            modFlags << "--" + opt->m_longFlag + "=" + cfgVal;
      }
   }
   return modFlags;
}


/* Creates this tool's ToolView window,
   and sets up and connections between them */
ToolView* Memcheck::createView( QWidget* parent )
{
   m_view = new MemcheckView( parent, this->name() );

   /* signals view --> tool */
   connect( m_view, SIGNAL(saveLogFile()),
            this, SLOT(fileSaveDialog()) );

   /* signals tool --> view */
   connect( this, SIGNAL(running(bool)),
            m_view, SLOT(setState(bool)) );

   setRunState( VkRunState::STOPPED );
   return m_view;
}


/* outputs a message to the status bar. */
void Memcheck::statusMsg( QString hdr, QString msg ) 
{ 
   emit message( hdr + ": " + msg );
}


/* are we done and dusted?
   anything we need to check/do before being deleted/closed?
*/
bool Memcheck::queryDone()
{
   vk_assert( view() != 0 );

   /* if current process is not yet finished, ask user if they really
      want to close */
   if ( isRunning() ) {
      int ok = vkQuery( view(), "Process Running", "&Abort;&Cancel",
                        "<p>The current process is not yet finished.</p>"
                        "<p>Do you want to abort it ?</p>" );
      if ( ok == MsgBox::vkYes ) {
         bool stopped = stop();         /* abort */
         vk_assert( stopped );          // TODO: what todo if couldn't stop?
      } else if ( ok == MsgBox::vkNo ) {
         return false;                         /* continue */
      }
   }

   if (!queryFileSave())
      return false;     // not saved: procrastinate.

   return true;
}

/* if current output not saved, ask user if want to save
   returns false if not saved, but user wants to procrastinate.
*/
bool Memcheck::queryFileSave()
{
   vk_assert( view() != 0 );
   vk_assert( !isRunning() );

   /* currently loaded / parsed stuff is saved to tmp file - ask user
      if they want to save it to a 'real' file */
   if ( !m_fileSaved ) {
      int ok = vkQuery( view(), "Unsaved File", 
                        "&Save;&Discard;&Cancel",
                        "<p>The current output is not saved, "
                        " and will be deleted.<br/>"
                        "Do you want to save it ?</p>" );
      if ( ok == MsgBox::vkYes ) {            /* save */

         if ( !fileSaveDialog() ) {
            /* user clicked Cancel, but we already have the
               auto-fname saved anyway, so get outta here. */
            return false;
         }

      } else if ( ok == MsgBox::vkCancel ) {  /* procrastinate */
         return false;
      } else {                                /* discard */
         QFile::remove( m_saveFname );
         m_fileSaved = true;
      }
   }
   return true;
}


bool Memcheck::start( VkRunState::State rs, QStringList vgflags )
{
   bool ok = false;
   vk_assert( rs != VkRunState::STOPPED );
   vk_assert( !isRunning() );
  
   switch ( rs ) {
   case VkRunState::VALGRIND: { ok = runValgrind( vgflags ); break; }
   case VkRunState::TOOL1:    { ok = parseLogFile();         break; }
   case VkRunState::TOOL2:    { ok = mergeLogFiles();        break; }
   default:
      vk_assert_never_reached();
   }
   return ok;
}


bool Memcheck::stop()
{
   vk_assert( isRunning() );

   switch ( runState() ) {
   case VkRunState::VALGRIND: {
      if (m_vgproc && m_vgproc->isRunning() )
         m_vgproc->stop(); /* signal -> processDone() */
   } break;

   case VkRunState::TOOL1: { /* parse log */
      /* TODO: make log parsing a VkProcess.  This will allow
         - valkyrie to stay responsive
         - the ability to interrupt the process if taking too long */
      VK_DEBUG("TODO: Memcheck::stop(parse log)" );
   } break;

   case VkRunState::TOOL2: { /* merge logs */
      if (m_vgproc && m_vgproc->isRunning() )
         m_vgproc->stop(); /* signal -> processDone() */
   } break;

   default:
      vk_assert_never_reached();
   }

   return true;
}


/* if --vg-opt=<arg> was specified on the cmd-line, called by
   valkyrie->runTool(); if set via the run-button in the gui, 
   then MainWindow::run() calls valkyrie->runTool().  */
bool Memcheck::runValgrind( QStringList vgflags )
{
   m_saveFname = vk_mkstemp( vkConfig->logsDir() + "mc_log", "xml" );
   vk_assert( !m_saveFname.isEmpty() );

   /* fill in filename in flags list */
#if (QT_VERSION-0 >= 0x030200)
   vgflags.gres( "--log-file-exactly", "--log-file-exactly=" + m_saveFname );
#else // QT_VERSION < 3.2
   QStringList::iterator it_str = vgflags.find("--log-file-exactly");
   if (it_str != vgflags.end())
      (*it_str) += ("=" + m_saveFname);
#endif
  
   setRunState( VkRunState::VALGRIND );
   m_fileSaved = false;
   statusMsg( "Memcheck", "Running ... " );

   bool ok = startProcess( vgflags );

   if (!ok) {
      statusMsg( "Memcheck", "Failed" );
      m_fileSaved = true;
      setRunState( VkRunState::STOPPED );
   }
   return ok;
}


/* Parse log file given by [valkyrie::view-log] entry.
   Called by valkyrie->runTool() if cmdline --view-log=<file> specified.
   MemcheckView::openLogFile() if gui parse-log selected.
   If 'checked' == true, file perms/format has already been checked */
bool Memcheck::parseLogFile()
{
   vk_assert( view() != 0 );

   QString log_file = vkConfig->rdEntry( "view-log", "valkyrie" );
   statusMsg( "Parsing", log_file );

   /* check this is a valid file, and has the right perms */
   int errval = PARSED_OK;
   QString ret_file = fileCheck( &errval, log_file, true, false );
   if ( errval != PARSED_OK ) {
      vkError( view(), "File Error", "%s: \n\"%s\"", 
               parseErrString(errval),
               escapeEntities(log_file).latin1() );
      return false;
   }
   log_file = ret_file;
  
   /* fileSaved is always true here 'cos we are just parsing a file
      which already exists on disk */
   m_fileSaved = true;
   setRunState( VkRunState::TOOL1 );

   /* Could be a very large file, so at least get ui up-to-date now */
   qApp->processEvents( 1000/*max msecs*/ );

   /* Parse the log */
   VgLogReader vgLogFileReader( view()->vgLogPtr() );
   bool success = vgLogFileReader.parse( log_file );
   if (!success) {
      VgLogHandler* hnd = vgLogFileReader.handler();
      statusMsg( "Parsing", "Error" );
      vkError( view(), "XML Parse Error",
               "<p>%s</p>", escapeEntities(hnd->fatalMsg()).latin1() );
   }

   if (success) {
      m_saveFname = log_file;
      statusMsg( "Loaded", log_file );
   } else {
      statusMsg( "Parse failed", log_file );
   }
   setRunState( VkRunState::STOPPED );
   return success;
}


/* if --merge=<file_list> was specified on the cmd-line, called by
   valkyrie->runTool(); if set via the open-file-dialog in the gui,
   called by MemcheckView::openMergeFile().  either way, the value in
   [valkyrie:merge] is what we need to know */
bool Memcheck::mergeLogFiles()
{
   QString fname_logList = vkConfig->rdEntry( "merge", "valkyrie" );
   statusMsg( "Merging logs in file-list", fname_logList );
 
   m_saveFname = vk_mkstemp( vkConfig->logsDir() + "mc_merged", "xml" );
   vk_assert( !m_saveFname.isEmpty() );

   QStringList flags;
   flags << vkConfig->rdEntry( "merge-exec","valkyrie");
   flags << "-f";
   flags << fname_logList;
   flags << "-o";
   flags << m_saveFname;

   setRunState( VkRunState::TOOL2 );
   m_fileSaved = false;
   statusMsg( "Merge Logs", "Running ... " );

   bool ok = startProcess( flags );

   if (!ok) {
      statusMsg( "Merge Logs", "Failed" );
      m_fileSaved = true;
      setRunState( VkRunState::STOPPED );
   }
   return ok;
}


/* Run a VKProcess, as given by 'flags'.
   Reads ouput from file, loading this to the listview.
*/
bool Memcheck::startProcess( QStringList flags )
{
   //   vkPrint("Memcheck::startProcess()");
   //   for ( unsigned int i=0; i<flags.count(); i++ )
   //      vkPrint("flag[%d] --> %s", i, flags[i].latin1() );
   vk_assert( view() != 0 );

   /* new m_vgreader - view() may be recreated, so need up-to-date ptr */
   vk_assert( m_vgreader == 0 );
   m_vgreader = new VgLogReader( view()->vgLogPtr() );

   /* start the log parse - nothing written yet tho */
   if (!m_vgreader->parse( m_saveFname, true )) {
      QString errMsg = m_vgreader->handler()->fatalMsg();
      VK_DEBUG("m_vgreader failed to start parsing empty log\n");
      vkError( view(), "Process Startup Error",
               "<p>Failed to start XML parser:<br>%s</p>",
               errMsg.ascii() );
      goto failed_startup;
   }

   /* start a new process, listening on exit signal */
   vk_assert( m_vgproc == 0 );
   m_vgproc = new VKProcess( flags, this );
   connect( m_vgproc, SIGNAL(processExited()),
            this, SLOT(processDone()) );

   /* don't need to talk/listen to forked process,
      so don't let it hijack stdin/out/err for socket fd's */
   m_vgproc->setCommunication( 0 );

   if ( !m_vgproc->start() ) {
      VK_DEBUG("process failed to start");
      QString path_errmsg = (runState() == VkRunState::VALGRIND)
         ? "Please verify the path to Valgrind in Options::Valkyrie."
         : ""; /* TODO: same for vk_logmerge... and provide option widgets to update path... */
      vkError( view(), "Process Startup Error",
               "<p>Failed to start process:<br>%s<br><br>%s</p>",
               flags.join(" ").latin1(),
               path_errmsg.latin1() );
      goto failed_startup;
   }

   /* poll log for latest data */
   if (!m_logpoller->start()) {
      QString errMsg = m_vgreader->handler()->fatalMsg();
      VK_DEBUG("m_logpoller failed to start\n");
      vkError( view(), "Process Startup Error",
               "<p>Failed to start log poller.</p>" );
      goto failed_startup;
   }

   //  vkPrint(" - END MC::startProcess()" );
   return true;

 failed_startup:
   VK_DEBUG("failed_startup: '%s'", flags.join(" ").latin1());
   if (m_logpoller != 0) {
      m_logpoller->stop();
   }
   if (m_vgreader != 0) {
      delete m_vgreader;
      m_vgreader = 0;
   }
   if (m_vgproc != 0) {
      delete m_vgproc;
      m_vgproc = 0;
   } 
   return false;
}


/* Process exited:
    - self / external signal / user via 'stop()' / 
    - terminated from readVgLog because of an xml parse error
   Stops logfile polling, checks xml parsing for errors,
   checks exitstatus, cleans up.
*/
void Memcheck::processDone()
{
   //   vkPrint("Memcheck::processDone()");
   vk_assert( m_vgproc != 0 );
   vk_assert( m_vgreader != 0 );
   vk_assert( m_logpoller != 0 );
   bool runError = false;

   /* stop polling logfile ------------------------------------------ */
   m_logpoller->stop();

   /* deal with log reader ------------------------------------------ */
   /* if not finished && no error, try reading log data one last time */
   if (!m_vgreader->handler()->finished() && 
       m_vgreader->handler()->fatalMsg().isEmpty())
      readVgLog();

   /* did log parsing go ok? */
   QString fatalMsg = m_vgreader->handler()->fatalMsg();
   if ( !fatalMsg.isEmpty() ) {
      /* fatal log error... */
      runError = true;
      //      vkPrint(" - Memcheck::processDone(): fatal error");

      if (runState() == VkRunState::VALGRIND) {
         statusMsg( "Memcheck", "Error parsing output log" );
         vkError( view(), "XML Parse Error",
                  "<p>Error parsing Valgrind XML output:<br>%s</p>",
                  str2html( fatalMsg ).latin1() );
      } else {
         statusMsg( "Merge Logs", "Error parsing output log" );
         vkError( view(), "Parse Error",
                  "<p>Error parsing output log</p>" );
      }
   } else if ( !m_vgreader->handler()->finished() ) {
      /* no fatal error, but STILL not reached end of log, either:
         - valgrind xml output not completed properly
         - merge failed */
      runError = true;
      //      vkPrint(" - Memcheck::processDone(): parsing STILL not finished");
            
      if (runState() == VkRunState::VALGRIND) {
         statusMsg( "Memcheck", "Error - incomplete output log" );
         vkError( view(), "XML Parse Error",
                  "<p>Valgrind XML output is incomplete</p>" );
      } else {
         statusMsg( "Merge Logs", "Error - incomplete output log" );
         vkError( view(), "Parse Error",
                  "<p>Failed to parse merge result</p>" );
      }
   }

   /* check process exit status
      - valgrind might have bombed ---------------------------------- */
   bool exitStatus = m_vgproc->exitStatus();
   if (exitStatus != 0) {
      //      vkPrint(" - Memcheck::processDone(): process failed (%d)", exitStatus);
      if (runState() == VkRunState::VALGRIND) {
         vkError( view(), "Run Error",
                  "<p>Process exited with return value %d.<br> \
                      This is likely to simply be the client program \
                      return value.  If, however, you suspect Valgrind \
                      itself may have crashed, please 'Save Log' and \
                      examine for details.</p>", exitStatus);
      } else {
         vkError( view(), "Parse Error",
                  "<p>Merge process exited with return value %d.<br> \
                      Please check the terminal for error messages.</p>",
                  exitStatus);
      }
   } else {
      //      vkPrint(" - Memcheck::processDone(): process exited ok");      
   }

   /* cleanup ------------------------------------------------------- */
   delete m_vgreader;
   m_vgreader = 0;
   delete m_vgproc;
   m_vgproc = 0;


   /* we're done. --------------------------------------------------- */
   if (!runError) { /* (else we've already set an status error message) */
      if (runState() == VkRunState::VALGRIND)
         statusMsg( "Memcheck", "Finished" );
      else
         statusMsg( "Merge Logs", "Finished" );
   }

   setRunState( VkRunState::STOPPED );
   //   vkPrint("Memcheck::processDone(): DONE.\n");
}


/* Read memcheck / logmerge xml output
   Called by
    - m_logpoller signals
    - processDone() if one last data read needed.
*/
void Memcheck::readVgLog()
{
   //   vkPrint("Memcheck::readVgLog()");
   vk_assert( view() != 0 );
   vk_assert( m_vgreader != 0 );
   vk_assert( m_vgproc != 0 );

   /* Try reading some more data */
   if ( !m_vgreader->parseContinue()) {
      /* Parsing failed: stop m_vgproc, if running */
      if (m_vgproc->isRunning())
         m_vgproc->stop();  /* signal -> processDone() */
   }
}


/* brings up a fileSaveDialog until successfully saved,
   or user pressed Cancel.
   if fname.isEmpty, ask user for a name first.
   returns false on user pressing Cancel, else true.
*/
bool Memcheck::fileSaveDialog( QString fname/*=QString()*/ )
{
   vk_assert( view() != 0 );

   QFileDialog dlg;
   dlg.setShowHiddenFiles( true );
   QString flt = "XML Files (*.xml);;Log Files (*.log.*);;All Files (*)";
   QString cptn = "Save Log File As";

   /* Ask fname if don't have one already */
   if ( fname.isEmpty() ) {
      /* start dlg in dir of last saved logfile */
      QString start_path = QFileInfo( m_saveFname ).dirPath();
      fname = dlg.getSaveFileName( start_path, flt, view(), "fsdlg", cptn );
      if ( fname.isEmpty() )
         return false;
   }

   /* try to save file until succeed, or user Cancels */
   while ( !saveParsedOutput( fname ) ) {
      QString start_path = QFileInfo( fname ).dirPath();
      fname = dlg.getSaveFileName( start_path, flt, view(), "fsdlg", cptn );
      if ( fname.isEmpty() )   /* Cancelled */
         return false;
   }

   return true;
}

/* Save to file
   - we already have everything in m_saveFname logfile, so just copy that
*/
bool Memcheck::saveParsedOutput( QString& fname )
{
   //vkPrint("saveParsedOutput(%s)", fname.latin1() );
   vk_assert( view() != 0 );
   vk_assert( !fname.isEmpty() );

   /* make sure path is absolute */
   fname = QFileInfo( fname ).absFilePath();

   /* if this filename already exists, check if we should over-write it */
   if ( QFile::exists( fname ) ) {
      int ok = vkQuery( view(), 2, "Overwrite File",
                        "<p>Over-write existing file '%s' ?</p>", 
                        fname.latin1() );
      if ( ok == MsgBox::vkNo ) {
         /* nogo: return and try again */
         return false;
      }
   }

   /* save log (=copy/rename) */
   bool ok;
   if (!m_fileSaved) {
      /* first save after a run, so just rename m_saveFname => fname */
      //vkPrint("renaming: '%s' -> '%s'", m_saveFname.latin1(), fname.latin1() );
      ok = QDir().rename( m_saveFname, fname );
   } else {
      /* we've saved once already: must now copy m_saveFname => fname */
      //vkPrint("copying: '%s' -> '%s'", m_saveFname.latin1(), fname.latin1() );
      QUrlOperator *op = new QUrlOperator();
      op->copy( m_saveFname, fname, false, false ); 
      /* TODO: check copied ok */
      ok = true;
   }
   if (ok) {
      m_saveFname = fname;
      m_fileSaved = true;
      statusMsg( "Saved", m_saveFname );
   } else {
      /* nogo: return and try again */
      vkInfo( view(), "Save Failed", 
              "<p>Failed to save file to '%s'",  fname.latin1() );
      statusMsg( "Failed Save", m_saveFname );
   }
   return ok;
}

