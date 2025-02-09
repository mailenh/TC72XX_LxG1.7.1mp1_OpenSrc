/******************************************************************************
 *
 * $Id: docparser.cpp,v 1.2 2014/11/19 09:12:47 wtchen Exp $
 *
 *
 * Copyright (C) 1997-2006 by Dimitri van Heesch.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation under the terms of the GNU General Public License is hereby 
 * granted. No representations are made about the suitability of this software 
 * for any purpose. It is provided "as is" without express or implied warranty.
 * See the GNU General Public License for more details.
 *
 * Documents produced by Doxygen are derivative works derived from the
 * input used in their production; they are not affected by this license.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <qfile.h>
#include <qfileinfo.h>
#include <qcstring.h>
#include <qstack.h>
#include <qdict.h>
#include <qregexp.h>
#include <ctype.h>

#include "doxygen.h"
#include "debug.h"
#include "util.h"
#include "pagedef.h"

#include "docparser.h"
#include "doctokenizer.h"
#include "cmdmapper.h"
#include "printdocvisitor.h"
#include "message.h"
#include "section.h"
#include "searchindex.h"
#include "language.h"

// debug off
#define DBG(x) do {} while(0)

// debug to stdout
//#define DBG(x) printf x

// debug to stderr
//#define myprintf(x...) fprintf(stderr,x)
//#define DBG(x) myprintf x

#define INTERNAL_ASSERT(x) do {} while(0)
//#define INTERNAL_ASSERT(x) if (!(x)) DBG(("INTERNAL_ASSERT(%s) failed retval=0x%x: file=%s line=%d\n",#x,retval,__FILE__,__LINE__)); 

//---------------------------------------------------------------------------

static const char *sectionLevelToName[] = 
{
  "page",
  "section",
  "subsection",
  "subsubsection",
  "paragraph"
};

//---------------------------------------------------------------------------

// global variables during a call to validatingParseDoc
static bool         g_hasParamCommand;
static bool         g_hasReturnCommand;
static MemberDef *  g_memberDef;
static QDict<void>  g_paramsFound;
static bool         g_isExample;
static QCString     g_exampleName;
static SectionDict *g_sectionDict;

static QCString     g_searchUrl;

// include file state
static QString g_includeFileText;
static uint     g_includeFileOffset;
static uint     g_includeFileLength;

// parser state
static QString                g_context;
static bool                   g_inSeeBlock;
static bool                   g_insideHtmlLink;
static QStack<DocNode>        g_nodeStack;
static QStack<DocStyleChange> g_styleStack;
static QStack<DocStyleChange> g_initialStyleStack;
static QList<Definition>      g_copyStack;
static QString                g_fileName;
static QString                g_relPath;

struct DocParserContext
{
  QString context;
  bool inSeeBlock;
  bool insideHtmlLink;
  QStack<DocNode> nodeStack;
  QStack<DocStyleChange> styleStack;
  QStack<DocStyleChange> initialStyleStack;
  QList<Definition> copyStack;
  MemberDef *memberDef;
  QString fileName;
  QString relPath;
};

static QStack<DocParserContext> g_parserStack;

//---------------------------------------------------------------------------

static void docParserPushContext()
{
  doctokenizerYYpushContext();
  DocParserContext *ctx   = new DocParserContext;
  ctx->context            = g_context;
  ctx->inSeeBlock         = g_inSeeBlock;
  ctx->insideHtmlLink     = g_insideHtmlLink;
  ctx->nodeStack          = g_nodeStack;
  ctx->styleStack         = g_styleStack;
  ctx->initialStyleStack  = g_initialStyleStack;
  ctx->copyStack          = g_copyStack;
  ctx->fileName           = g_fileName;
  ctx->relPath            = g_relPath;
  g_parserStack.push(ctx);
}

static void docParserPopContext()
{
  DocParserContext *ctx = g_parserStack.pop();
  g_context             = ctx->context;
  g_inSeeBlock          = ctx->inSeeBlock;
  g_insideHtmlLink      = ctx->insideHtmlLink;
  g_nodeStack           = ctx->nodeStack;
  g_styleStack          = ctx->styleStack;
  g_initialStyleStack   = ctx->initialStyleStack;
  g_copyStack           = ctx->copyStack;
  g_fileName            = ctx->fileName;
  g_relPath             = ctx->relPath;
  delete ctx;
  doctokenizerYYpopContext();
}

//---------------------------------------------------------------------------

/*! search for an image in the imageNameDict and if found
 * copies the image to the output directory (which depends on the \a type
 * parameter).
 */
static QCString findAndCopyImage(const char *fileName,DocImage::Type type)
{
  QCString result;
  bool ambig;
  FileDef *fd;
  //printf("Search for %s\n",fileName);
  if ((fd=findFileDef(Doxygen::imageNameDict,fileName,ambig)))
  {
    QFile inImage(QString(fd->absFilePath().data()));
    if (inImage.open(IO_ReadOnly))
    {
      result = fileName;
      int i;
      if ((i=result.findRev('/'))!=-1 || (i=result.findRev('\\'))!=-1)
      {
	result = result.right(result.length()-i-1);
      }
      //printf("fileName=%s result=%s\n",fileName,result.data());
      QCString outputDir;
      switch(type)
      {
        case DocImage::Html: 
	  if (!Config_getBool("GENERATE_HTML")) return result;
	  outputDir = Config_getString("HTML_OUTPUT");
	  break;
        case DocImage::Latex: 
	  if (!Config_getBool("GENERATE_LATEX")) return result;
	  outputDir = Config_getString("LATEX_OUTPUT");
	  break;
        case DocImage::Rtf:
	  if (!Config_getBool("GENERATE_RTF")) return result;
	  outputDir = Config_getString("RTF_OUTPUT");
	  break;
      }
      QCString outputFile = outputDir+"/"+result;
      QFile outImage(QString(outputFile.data()));
      if (outImage.open(IO_WriteOnly)) // copy the image
      {
	char *buffer = new char[inImage.size()];
	inImage.readBlock(buffer,inImage.size());
	outImage.writeBlock(buffer,inImage.size());
	outImage.flush();
	delete buffer;
      }
      else
      {
	warn_doc_error(g_fileName,doctokenizerYYlineno,
	    "Warning: could not write output image %s",outputFile.data());
      }
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,
	  "Warning: could not open image %s",fileName);
    }

    if (type==DocImage::Latex && Config_getBool("USE_PDFLATEX") && 
	fd->name().right(4)==".eps"
       )
    { // we have an .eps image in pdflatex mode => convert it to a pdf.
      QCString outputDir = Config_getString("LATEX_OUTPUT");
      QCString baseName  = fd->name().left(fd->name().length()-4);
      QCString epstopdfArgs(4096);
      epstopdfArgs.sprintf("\"%s/%s.eps\" --outfile=\"%s/%s.pdf\"",
                           outputDir.data(), baseName.data(),
			   outputDir.data(), baseName.data());
      if (iSystem("epstopdf",epstopdfArgs)!=0)
      {
	err("Error: Problems running epstopdf. Check your TeX installation!\n");
      }
      return baseName;
    }
  }
  else if (ambig)
  {
    QCString text;
    text.sprintf("Warning: image file name %s is ambigious.\n",fileName);
    text+="Possible candidates:\n";
    text+=showFileDefMatches(Doxygen::imageNameDict,fileName);
    warn_doc_error(g_fileName,doctokenizerYYlineno,text);
  }
  else
  {
    result=fileName;
    if (result.left(5)!="http:" && result.left(6)!="https:")
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,
           "Warning: image file %s is not found in IMAGE_PATH: "  
	   "assuming external image.",fileName
          );
    }
  }
  return result;
}

/*! Collects the parameters found with \@param or \@retval commands
 *  in a global list g_paramsFound. If \a isParam is set to TRUE
 *  and the parameter is not an actual parameter of the current
 *  member g_memberDef, then a warning is raised (unless warnings
 *  are disabled altogether).
 */
static void checkArgumentName(const QString &name,bool isParam)
{                
  if (!Config_getBool("WARN_IF_DOC_ERROR")) return;
  if (g_memberDef==0) return; // not a member
  ArgumentList *al=g_memberDef->isDocsForDefinition() ? 
		   g_memberDef->argumentList() :
                   g_memberDef->declArgumentList();
  if (al==0) return; // no argument list

  static QRegExp re("[a-zA-Z0-9_]+\\.*");
  int p=0,i=0,l;
  while ((i=re.match(name,p,&l))!=-1) // to handle @param x,y
  {
    QString aName=name.mid(i,l);
    //printf("aName=`%s'\n",aName.data());
    ArgumentListIterator ali(*al);
    Argument *a;
    bool found=FALSE;
    for (ali.toFirst();(a=ali.current());++ali)
    {
      QString argName = g_memberDef->isDefine() ? a->type : a->name;
      //printf("argName=`%s'\n",argName.data());
      if (argName.right(3)=="...") argName=argName.left(argName.length()-3);
      if (aName==argName) 
      {
	//printf("adding `%s'\n",aName.data());
	g_paramsFound.insert(aName,(void *)(0x8));
	found=TRUE;
	break;
      }
    }
    if (!found && isParam)
    {
      //printf("member type=%d\n",memberDef->memberType());
      QString scope=g_memberDef->getScopeString();
      if (!scope.isEmpty()) scope+="::"; else scope="";
      QString inheritedFrom = "";
      QString docFile = g_memberDef->docFile();
      int docLine = g_memberDef->docLine();
      MemberDef *inheritedMd = g_memberDef->inheritsDocsFrom();
      if (inheritedMd) // documentation was inherited
      {
        inheritedFrom.sprintf(" inherited from member %s at line "
            "%d in file %s",inheritedMd->name().data(),
            inheritedMd->docLine(),inheritedMd->docFile().data());
        docFile = g_memberDef->getDefFileName();
        docLine = g_memberDef->getDefLine();
        
      }
      warn_doc_error(docFile,docLine,
	  "Warning: argument `%s' of command @param "
	  "is not found in the argument list of %s%s%s%s",
	  aName.data(),scope.data(),g_memberDef->name().data(),
	  argListToString(al).data(),inheritedFrom.data());
    }
    p=i+l;
  }
}

/*! Checks if the parameters that have been specified using \@param are
 *  indeed all paramters.
 *  Must be called after checkArgumentName() has been called for each
 *  argument.
 */
static void checkUndocumentedParams()
{
  if (g_memberDef && g_hasParamCommand && Config_getBool("WARN_IF_DOC_ERROR"))
  {
    ArgumentList *al=g_memberDef->isDocsForDefinition() ? 
      g_memberDef->argumentList() :
      g_memberDef->declArgumentList();
    if (al)
    {
      ArgumentListIterator ali(*al);
      Argument *a;
      bool found=FALSE;
      for (ali.toFirst();(a=ali.current());++ali)
      {
        QString argName = g_memberDef->isDefine() ? a->type : a->name;
        if (argName.right(3)=="...") argName=argName.left(argName.length()-3);
        if (!argName.isEmpty() && g_paramsFound.find(argName)==0 && a->docs.isEmpty()) 
        {
          found = TRUE;
          break;
        }
      }
      if (found)
      {
        QString errMsg=
            "Warning: The following parameters of "+
            QString(g_memberDef->qualifiedName()) + 
            QString(argListToString(al)) +
            " are not documented:\n";
        for (ali.toFirst();(a=ali.current());++ali)
        {
          QString argName = g_memberDef->isDefine() ? a->type : a->name;
          if (!argName.isEmpty() && g_paramsFound.find(argName)==0) 
          {
            errMsg+="  parameter "+argName+"\n";
          }
        }
        if (g_memberDef->inheritsDocsFrom())
        {
           warn_doc_error(g_memberDef->getDefFileName(),g_memberDef->getDefLine(),errMsg);
        }
        else
        {
           warn_doc_error(g_memberDef->docFile(),g_memberDef->docLine(),errMsg);
        }
      }
    }
  }
}

/*! Check if a member has documentation for its parameter and or return
 *  type, if applicable. If found this will be stored in the member, this
 *  is needed as a member can have brief and detailed documentation, while
 *  only one of these needs to document the parameters.
 */
static void detectNoDocumentedParams()
{
  if (g_memberDef && Config_getBool("WARN_NO_PARAMDOC"))
  {
    ArgumentList *al     = g_memberDef->argumentList();
    ArgumentList *declAl = g_memberDef->declArgumentList();
    QString returnType   = g_memberDef->typeString();

    if (!g_memberDef->hasDocumentedParams() &&
        g_hasParamCommand)
    {
      //printf("%s->setHasDocumentedParams(TRUE);\n",g_memberDef->name().data());
      g_memberDef->setHasDocumentedParams(TRUE);
    }
    else if (!g_memberDef->hasDocumentedParams())
    {
      bool allDoc=TRUE; // no paramater => all parameters are documented
      if ( // member has parameters
             al &&          // but the member has a parameter list
             al->count()>0  // with at least one parameter (that is not void)
         )
      {
        ArgumentListIterator ali(*al);
        Argument *a;

        // see if all parameters have documentation
        for (ali.toFirst();(a=ali.current()) && allDoc;++ali)
        {
          if (!a->name.isEmpty() && a->type!="void")
          {
            allDoc = !a->docs.isEmpty();
          }
          //printf("a->type=%s a->name=%s doc=%s\n",
          //        a->type.data(),a->name.data(),a->docs.data());
        }
        if (!allDoc && declAl) // try declaration arguments as well
        {
          allDoc=TRUE;
          ArgumentListIterator ali(*declAl);
          Argument *a;
          for (ali.toFirst();(a=ali.current()) && allDoc;++ali)
          {
            if (!a->name.isEmpty() && a->type!="void")
            {
              allDoc = !a->docs.isEmpty();
            }
            //printf("a->name=%s doc=%s\n",a->name.data(),a->docs.data());
          }
        }
      }
      if (allDoc) 
      {
        //printf("%s->setHasDocumentedParams(TRUE);\n",g_memberDef->name().data());
        g_memberDef->setHasDocumentedParams(TRUE);
      }
    }
    //printf("Member %s hasReturnCommand=%d\n",g_memberDef->name().data(),g_hasReturnCommand);
    if (!g_memberDef->hasDocumentedReturnType() && // docs not yet found
        g_hasReturnCommand)
    {
      g_memberDef->setHasDocumentedReturnType(TRUE);
    }
    else if ( // see if return needs to documented 
        g_memberDef->hasDocumentedReturnType() ||
        returnType.isEmpty() ||          // empty return type
        returnType.find("void")!=-1  ||  // void return type
        !g_memberDef->isConstructor() || // a constructor
        !g_memberDef->isDestructor()     // or destructor
       )
    {
      g_memberDef->setHasDocumentedReturnType(TRUE);
    }
       
  }
}


//---------------------------------------------------------------------------

/*! Strips known html and tex extensions from \a text. */
static QString stripKnownExtensions(const char *text)
{
  QString result=text;
  if (result.right(4)==".tex")
  {
    result=result.left(result.length()-4);
  }
  else if (result.right(Doxygen::htmlFileExtension.length())==
         QString(Doxygen::htmlFileExtension)) 
  {
    result=result.left(result.length()-Doxygen::htmlFileExtension.length());
  }
  return result;
}


//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a preformatted node */
static bool insidePRE(DocNode *n)
{
  while (n)
  {
    if (n->isPreformatted()) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a html list item node */
static bool insideLI(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlListItem) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a unordered html list node */
static bool insideUL(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlList && 
        ((DocHtmlList *)n)->type()==DocHtmlList::Unordered) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a ordered html list node */
static bool insideOL(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlList && 
        ((DocHtmlList *)n)->type()==DocHtmlList::Ordered) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

///*! Returns TRUE iff node n is a child of a language node */
//static bool insideLang(DocNode *n)
//{
//  while (n)
//  {
//    if (n->kind()==DocNode::Kind_Language) return TRUE;
//    n=n->parent();
//  }
//  return FALSE;
//}


//---------------------------------------------------------------------------

/*! Looks for a documentation block with name commandName in the current
 *  context (g_context). The resulting documentation string is
 *  put in pDoc, the definition in which the documentation was found is
 *  put in pDef.
 *  @retval TRUE if name was found.
 *  @retval FALSE if name was not found.
 */
static bool findDocsForMemberOrCompound(const char *commandName,
                                 QString *pDoc,
                                 QString *pBrief,
                                 Definition **pDef)
{
  //printf("findDocsForMemberOrCompound(%s)\n",commandName);
  *pDoc="";
  *pBrief="";
  *pDef=0;
  QString cmdArg=substitute(commandName,"#","::");
  int l=cmdArg.length();
  if (l==0) return FALSE;

  int funcStart=cmdArg.find('(');
  if (funcStart==-1) funcStart=l;

  QString name=removeRedundantWhiteSpace(cmdArg.left(funcStart).latin1());
  QString args=cmdArg.right(l-funcStart);

  // try if the link is to a member
  MemberDef    *md=0;
  ClassDef     *cd=0;
  FileDef      *fd=0;
  NamespaceDef *nd=0;
  GroupDef     *gd=0;
  PageDef      *pd=0;
  bool found = getDefs(
      g_context.find('.')==-1?g_context.latin1():"", // `find('.') is a hack to detect files
      name.latin1(),
      args.isEmpty()?0:args.latin1(),
      md,cd,fd,nd,gd,FALSE,0,TRUE);
  //printf("found=%d context=%s name=%s\n",found,g_context.data(),name.data());
  if (found && md)
  {
    *pDoc=md->documentation();
    *pBrief=md->briefDescription();
    *pDef=md;
    return TRUE;
  }


  int scopeOffset=g_context.length();
  do // for each scope
  {
    QString fullName=cmdArg;
    if (scopeOffset>0)
    {
      fullName.prepend(g_context.left(scopeOffset)+"::");
    }
    //printf("Trying fullName=`%s'\n",fullName.data());

    // try class, namespace, group, page, file reference
    cd = Doxygen::classSDict[fullName];
    if (cd) // class 
    {
      *pDoc=cd->documentation();
      *pBrief=cd->briefDescription();
      *pDef=cd;
      return TRUE;
    }
    nd = Doxygen::namespaceSDict[fullName];
    if (nd) // namespace
    {
      *pDoc=nd->documentation();
      *pBrief=nd->briefDescription();
      *pDef=nd;
      return TRUE;
    }
    gd = Doxygen::groupSDict[cmdArg];
    if (gd) // group
    {
      *pDoc=gd->documentation();
      *pBrief=gd->briefDescription();
      *pDef=gd;
      return TRUE;
    }
    pd = Doxygen::pageSDict->find(cmdArg);
    if (pd) // page
    {
      *pDoc=pd->documentation();
      *pBrief=pd->briefDescription();
      *pDef=pd;
      return TRUE;
    }
    bool ambig;
    fd = findFileDef(Doxygen::inputNameDict,cmdArg,ambig);
    if (fd && !ambig) // file
    {
      *pDoc=fd->documentation();
      *pBrief=fd->briefDescription();
      *pDef=fd;
      return TRUE;
    }

    if (scopeOffset==0)
    {
      scopeOffset=-1;
    }
    else
    {
      scopeOffset = g_context.findRev("::",scopeOffset-1);
      if (scopeOffset==-1) scopeOffset=0;
    }
  } while (scopeOffset>=0);

  
  return FALSE;
}
//---------------------------------------------------------------------------

// forward declaration
static bool defaultHandleToken(DocNode *parent,int tok, 
                               QList<DocNode> &children,bool
                               handleWord=TRUE);


static int handleStyleArgument(DocNode *parent,QList<DocNode> &children,
                               const QString &cmdName)
{
  DBG(("handleStyleArgument(%s)\n",cmdName.data()));
  QString tokenName = g_token->name;
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
	cmdName.data());
    return tok;
  }
  while ((tok=doctokenizerYYlex()) && 
          tok!=TK_WHITESPACE && 
          tok!=TK_NEWPARA &&
          tok!=TK_LISTITEM && 
          tok!=TK_ENDLIST
        )
  {
    static QRegExp specialChar("[.,|()\\[\\]:;\\?]");
    if (tok==TK_WORD && g_token->name.length()==1 && 
        g_token->name.find(specialChar)!=-1)
    {
      // special character that ends the markup command
      return tok;
    }
    if (!defaultHandleToken(parent,tok,children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command \\%s as the argument of a \\%s command",
	       g_token->name.data(),cmdName.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found while handling command %s",
               g_token->name.data(),cmdName.data());
          break;
        case TK_HTMLTAG:
          if (insideLI(parent) && Mappers::htmlTagMapper->map(g_token->name) && g_token->endTag)
          { // ignore </li> as the end of a style command
            continue; 
          }
          return tok;
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s while handling command %s",
	       tokToString(tok),cmdName.data());
          break;
      }
      break;
    }
  }
  DBG(("handleStyleArgument(%s) end tok=%x\n",cmdName.data(),tok));
  return (tok==TK_NEWPARA || tok==TK_LISTITEM || tok==TK_ENDLIST
         ) ? tok : RetVal_OK; 
}

/*! Called when a style change starts. For instance a \<b\> command is
 *  encountered.
 */
static void handleStyleEnter(DocNode *parent,QList<DocNode> &children,
          DocStyleChange::Style s,const HtmlAttribList *attribs)
{
  DBG(("HandleStyleEnter\n"));
  DocStyleChange *sc= new DocStyleChange(parent,g_nodeStack.count(),s,TRUE,attribs);
  children.append(sc);
  g_styleStack.push(sc);
}

/*! Called when a style change ends. For instance a \</b\> command is
 *  encountered.
 */
static void handleStyleLeave(DocNode *parent,QList<DocNode> &children,
         DocStyleChange::Style s,const char *tagName)
{
  DBG(("HandleStyleLeave\n"));
  if (g_styleStack.isEmpty() ||                           // no style change
      g_styleStack.top()->style()!=s ||                   // wrong style change
      g_styleStack.top()->position()!=g_nodeStack.count() // wrong position
     )
  {
    if (g_styleStack.isEmpty())
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </%s> tag without matching <%s>",
          tagName,tagName);
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </%s> tag while expecting </%s>",
          tagName,g_styleStack.top()->styleString());
    }
  }
  else // end the section
  {
    DocStyleChange *sc= new DocStyleChange(parent,g_nodeStack.count(),s,FALSE);
    children.append(sc);
    g_styleStack.pop();
  }
}

/*! Called at the end of a paragraph to close all open style changes
 *  (e.g. a <b> without a </b>). The closed styles are pushed onto a stack
 *  and entered again at the start of a new paragraph.
 */
static void handlePendingStyleCommands(DocNode *parent,QList<DocNode> &children)
{
  if (!g_styleStack.isEmpty())
  {
    DocStyleChange *sc = g_styleStack.top();
    while (sc && sc->position()>=g_nodeStack.count()) 
    { // there are unclosed style modifiers in the paragraph
      children.append(new DocStyleChange(parent,g_nodeStack.count(),sc->style(),FALSE));
      g_initialStyleStack.push(sc);
      g_styleStack.pop();
      sc = g_styleStack.top();
    }
  }
}

static void handleInitialStyleCommands(DocPara *parent,QList<DocNode> &children)
{
  DocStyleChange *sc;
  while ((sc=g_initialStyleStack.pop()))
  {
    handleStyleEnter(parent,children,sc->style(),&sc->attribs());
  }
}

static int handleAHref(DocNode *parent,QList<DocNode> &children,const HtmlAttribList &tagHtmlAttribs)
{
  HtmlAttribListIterator li(tagHtmlAttribs);
  HtmlAttrib *opt;
  int index=0;
  int retval = RetVal_OK;
  for (li.toFirst();(opt=li.current());++li,++index)
  {
    if (opt->name=="name") // <a name=label> tag
    {
      if (!opt->value.isEmpty())
      {
        DocAnchor *anc = new DocAnchor(parent,opt->value,TRUE);
        children.append(anc);
        break; // stop looking for other tag attribs
      }
      else
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found <a> tag with name option but without value!");
      }
    }
    else if (opt->name=="href") // <a href=url>..</a> tag
    {
      // copy attributes
      HtmlAttribList attrList = tagHtmlAttribs;
      // and remove the href attribute
      bool result = attrList.remove(index);
      ASSERT(result);
      DocHRef *href = new DocHRef(parent,attrList,opt->value);
      children.append(href);
      g_insideHtmlLink=TRUE;
      retval = href->parse();
      g_insideHtmlLink=FALSE;
      break;
    }
    else // unsupported option for tag a
    {
    }
  }
  return retval;
}

const char *DocStyleChange::styleString() const
{
  switch (m_style)
  {
    case DocStyleChange::Bold:         return "b"; 
    case DocStyleChange::Italic:       return "em"; 
    case DocStyleChange::Code:         return "code"; 
    case DocStyleChange::Center:       return "center"; 
    case DocStyleChange::Small:        return "small"; 
    case DocStyleChange::Subscript:    return "subscript"; 
    case DocStyleChange::Superscript:  return "superscript"; 
    case DocStyleChange::Preformatted: return "pre"; 
    case DocStyleChange::Div:          return "div";
    case DocStyleChange::Span:         return "span";
  }
  return "<invalid>";
}

static void handleUnclosedStyleCommands()
{
  if (!g_initialStyleStack.isEmpty())
  {
    DocStyleChange *sc = g_initialStyleStack.top();
    g_initialStyleStack.pop();
    handleUnclosedStyleCommands();
    warn_doc_error(g_fileName,doctokenizerYYlineno,
             "Warning: end of comment block while expecting "
             "command </%s>",sc->styleString());
  }
}


static void handleLinkedWord(DocNode *parent,QList<DocNode> &children)
{
  Definition *compound=0;
  MemberDef  *member=0;
  QString name = linkToText(g_token->name,TRUE);
  int len = g_token->name.length();
  ClassDef *cd=0;
  //printf("handleLinkedWord(%s) g_context=%s\n",name.data(),g_context.data());
  if (!g_insideHtmlLink && 
      (resolveRef(g_context,g_token->name,g_inSeeBlock,&compound,&member)
       || (!g_context.isEmpty() &&  // also try with global scope
           resolveRef("",g_token->name,g_inSeeBlock,&compound,&member))
      )
     )
  {
    //printf("resolveRef %s = %p (linkable?=%d)\n",g_token->name.data(),member,member ? member->isLinkable() : FALSE);
    if (member && member->isLinkable()) // member link
    {
      if (member->isObjCMethod()) 
      {
        bool localLink = g_memberDef ? member->getClassDef()==g_memberDef->getClassDef() : FALSE;
        name = member->objCMethodName(localLink,g_inSeeBlock);
      }
      children.append(new 
          DocLinkedWord(parent,name,
            member->getReference(),
            member->getOutputFileBase(),
            member->anchor()
                       )
                     );
    }
    else if (compound->isLinkable()) // compound link
    {
      if (compound->definitionType()==Definition::TypeFile)
      {
        name=g_token->name;
      }
      else if (compound->definitionType()==Definition::TypeGroup)
      {
        name=((GroupDef*)compound)->groupTitle();
      }
      children.append(new 
          DocLinkedWord(parent,name,
                        compound->getReference(),
                        compound->getOutputFileBase(),
                        ""
                       )
                     );
    }
    else if (compound->definitionType()==Definition::TypeFile &&
             ((FileDef*)compound)->generateSourceFile()
            ) // undocumented file that has source code we can link to
    {
      children.append(new 
          DocLinkedWord(parent,g_token->name,
                         compound->getReference(),
                         compound->getSourceFileBase(),
                         ""
                       )
                     );
    }
    else // not linkable
    {
      children.append(new DocWord(parent,name));
    }
  }
  else if (!g_insideHtmlLink && len>1 && g_token->name.at(len-1)==':')
  {
    // special case, where matching Foo: fails to be an Obj-C reference, 
    // but Foo itself might be linkable.
    g_token->name=g_token->name.left(len-1);
    handleLinkedWord(parent,children);
    children.append(new DocWord(parent,":"));
  }
  else if (!g_insideHtmlLink && (cd=getClass(g_token->name+"-p")))
  {
    // special case 2, where the token name is not a class, but could
    // be a Obj-C protocol
    children.append(new 
        DocLinkedWord(parent,name,
          cd->getReference(),
          cd->getOutputFileBase(),
          ""));
  }
  else // normal non-linkable word
  {
    if (g_token->name.at(0)=='#')
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: explicit link request to '%s' could not be resolved",name.data());
    }
    children.append(new DocWord(parent,name));
  }
}

/* Helper function that deals with the most common tokens allowed in
 * title like sections. 
 * @param parent     Parent node, owner of the children list passed as 
 *                   the third argument. 
 * @param tok        The token to process.
 * @param children   The list of child nodes to which the node representing
 *                   the token can be added.
 * @param handleWord Indicates if word token should be processed
 * @retval TRUE      The token was handled.
 * @retval FALSE     The token was not handled.
 */
static bool defaultHandleToken(DocNode *parent,int tok, QList<DocNode> &children,bool
    handleWord)
{
  DBG(("token %s at %d",tokToString(tok),doctokenizerYYlineno));
  if (tok==TK_WORD || tok==TK_LNKWORD || tok==TK_SYMBOL || tok==TK_URL || 
      tok==TK_COMMAND || tok==TK_HTMLTAG
     )
  {
    DBG((" name=%s",g_token->name.data()));
  }
  DBG(("\n"));
reparsetoken:
  QString tokenName = g_token->name;
  switch (tok)
  {
    case TK_COMMAND: 
      switch (Mappers::cmdMapper->map(tokenName))
      {
        case CMD_BSLASH:
          children.append(new DocSymbol(parent,DocSymbol::BSlash));
          break;
        case CMD_AT:
          children.append(new DocSymbol(parent,DocSymbol::At));
          break;
        case CMD_LESS:
          children.append(new DocSymbol(parent,DocSymbol::Less));
          break;
        case CMD_GREATER:
          children.append(new DocSymbol(parent,DocSymbol::Greater));
          break;
        case CMD_AMP:
          children.append(new DocSymbol(parent,DocSymbol::Amp));
          break;
        case CMD_DOLLAR:
          children.append(new DocSymbol(parent,DocSymbol::Dollar));
          break;
        case CMD_HASH:
          children.append(new DocSymbol(parent,DocSymbol::Hash));
          break;
        case CMD_PERCENT:
          children.append(new DocSymbol(parent,DocSymbol::Percent));
          break;
        case CMD_EMPHASIS:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Italic,TRUE));
            tok=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Italic,FALSE));
            if (tok!=TK_WORD) children.append(new DocWhiteSpace(parent," "));
            if (tok==TK_NEWPARA) goto handlepara;
            else if (tok==TK_WORD || tok==TK_HTMLTAG) 
            {
	      DBG(("CMD_EMPHASIS: reparsing command %s\n",g_token->name.data()));
              goto reparsetoken;
            }
          }
          break;
        case CMD_BOLD:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Bold,TRUE));
            tok=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Bold,FALSE));
            if (tok!=TK_WORD) children.append(new DocWhiteSpace(parent," "));
            if (tok==TK_NEWPARA) goto handlepara;
            else if (tok==TK_WORD || tok==TK_HTMLTAG) 
            {
	      DBG(("CMD_BOLD: reparsing command %s\n",g_token->name.data()));
              goto reparsetoken;
            }
          }
          break;
        case CMD_CODE:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Code,TRUE));
            tok=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Code,FALSE));
            if (tok!=TK_WORD) children.append(new DocWhiteSpace(parent," "));
            if (tok==TK_NEWPARA) goto handlepara;
            else if (tok==TK_WORD || tok==TK_HTMLTAG) 
            {
	      DBG(("CMD_CODE: reparsing command %s\n",g_token->name.data()));
              goto reparsetoken;
            }
          }
          break;
        case CMD_HTMLONLY:
          {
            doctokenizerYYsetStateHtmlOnly();
            tok = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_context,g_token->verb,DocVerbatim::HtmlOnly,g_isExample,g_exampleName));
            if (tok==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: htmlonly section ended without end marker");
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_MANONLY:
          {
            doctokenizerYYsetStateManOnly();
            tok = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_context,g_token->verb,DocVerbatim::ManOnly,g_isExample,g_exampleName));
            if (tok==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: manonly section ended without end marker");
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_LATEXONLY:
          {
            doctokenizerYYsetStateLatexOnly();
            tok = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_context,g_token->verb,DocVerbatim::LatexOnly,g_isExample,g_exampleName));
            if (tok==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: latexonly section ended without end marker",doctokenizerYYlineno);
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_XMLONLY:
          {
            doctokenizerYYsetStateXmlOnly();
            tok = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_context,g_token->verb,DocVerbatim::XmlOnly,g_isExample,g_exampleName));
            if (tok==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: xmlonly section ended without end marker",doctokenizerYYlineno);
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_FORMULA:
          {
            DocFormula *form=new DocFormula(parent,g_token->id);
            children.append(form);
          }
          break;
        case CMD_ANCHOR:
          {
            tok=doctokenizerYYlex();
            if (tok!=TK_WHITESPACE)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
                  tokenName.data());
              break;
            }
            tok=doctokenizerYYlex();
            if (tok==0)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
                  "argument of command %s",tokenName.data());
              break;
            }
            else if (tok!=TK_WORD && tok!=TK_LNKWORD)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
                  tokToString(tok),tokenName.data());
              break;
            }
            DocAnchor *anchor = new DocAnchor(parent,g_token->name,FALSE);
            children.append(anchor);
          }
          break;
        case CMD_INTERNALREF:
          {
            tok=doctokenizerYYlex();
            if (tok!=TK_WHITESPACE)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
                  tokenName.data());
              break;
            }
            doctokenizerYYsetStateInternalRef();
            tok=doctokenizerYYlex(); // get the reference id
            DocInternalRef *ref=0;
            if (tok!=TK_WORD && tok!=TK_LNKWORD)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
                  tokToString(tok),tokenName.data());
              doctokenizerYYsetStatePara();
              break;
            }
            ref = new DocInternalRef(parent,g_token->name);
            children.append(ref);
            ref->parse();
            doctokenizerYYsetStatePara();
          }
          break;
        default:
          return FALSE;
      }
      break;
    case TK_HTMLTAG:
      {
        switch (Mappers::htmlTagMapper->map(tokenName))
        {
          case HTML_DIV:
            warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found <div> tag in heading\n");
            break;
          case HTML_PRE:
            warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found <pre> tag in heading\n");
            break;
          case HTML_BOLD:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Bold,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Bold,tokenName);
            }
            break;
          case HTML_CODE:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Code,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Code,tokenName);
            }
            break;
          case HTML_EMPHASIS:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Italic,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Italic,tokenName);
            }
            break;
          case HTML_SUB:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Subscript,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Subscript,tokenName);
            }
            break;
          case HTML_SUP:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Superscript,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Superscript,tokenName);
            }
            break;
          case HTML_CENTER:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Center,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Center,tokenName);
            }
            break;
          case HTML_SMALL:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Small,&g_token->attribs);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Small,tokenName);
            }
            break;
          default:
            return FALSE;
            break;
        }
      }
      break;
    case TK_SYMBOL: 
      {
        char letter='\0';
        DocSymbol::SymType s = DocSymbol::decodeSymbol(tokenName,&letter);
        if (s!=DocSymbol::Unknown)
        {
          children.append(new DocSymbol(parent,s,letter));
        }
        else
        {
          return FALSE;
        }
      }
      break;
    case TK_WHITESPACE: 
    case TK_NEWPARA: 
handlepara:
      if (insidePRE(parent) || !children.isEmpty())
      {
        children.append(new DocWhiteSpace(parent,g_token->chars));
      }
      break;
    case TK_LNKWORD: 
      if (handleWord)
      {
        handleLinkedWord(parent,children);
      }
      else
        return FALSE;
      break;
    case TK_WORD: 
      if (handleWord)
      {
        children.append(new DocWord(parent,g_token->name));
      }
      else
        return FALSE;
      break;
    case TK_URL:
      if (g_insideHtmlLink)
      {
        children.append(new DocWord(parent,g_token->name));
      }
      else
      {
        children.append(new DocURL(parent,g_token->name,g_token->isEMailAddr));
      }
      break;
    default:
      return FALSE;
  }
  return TRUE;
}


//---------------------------------------------------------------------------

DocSymbol::SymType DocSymbol::decodeSymbol(const QString &symName,char *letter)
{
  int l=symName.length();
  DBG(("decodeSymbol(%s) l=%d\n",symName.data(),l));
  if      (symName=="&copy;")  return DocSymbol::Copy;
  else if (symName=="&tm;")    return DocSymbol::Tm;
  else if (symName=="&reg;")   return DocSymbol::Reg;
  else if (symName=="&lt;")    return DocSymbol::Less;
  else if (symName=="&gt;")    return DocSymbol::Greater;
  else if (symName=="&amp;")   return DocSymbol::Amp;
  else if (symName=="&apos;")  return DocSymbol::Apos;
  else if (symName=="&quot;")  return DocSymbol::Quot;
  else if (symName=="&lsquo;") return DocSymbol::Lsquo;
  else if (symName=="&rsquo;") return DocSymbol::Rsquo;
  else if (symName=="&ldquo;") return DocSymbol::Ldquo;
  else if (symName=="&rdquo;") return DocSymbol::Rdquo;
  else if (symName=="&ndash;") return DocSymbol::Ndash;
  else if (symName=="&mdash;") return DocSymbol::Mdash;
  else if (symName=="&szlig;") return DocSymbol::Szlig;
  else if (symName=="&nbsp;")  return DocSymbol::Nbsp;
  else if (l==6 && symName.right(4)=="uml;")  
  {
    *letter=symName.at(1);
    return DocSymbol::Uml;
  }
  else if (l==8 && symName.right(6)=="acute;")  
  {
    *letter=symName.at(1);
    return DocSymbol::Acute;
  }
  else if (l==8 && symName.right(6)=="grave;")
  {
    *letter=symName.at(1);
    return DocSymbol::Grave;
  }
  else if (l==7 && symName.right(5)=="circ;")
  {
    *letter=symName.at(1);
    return DocSymbol::Circ;
  }
  else if (l==8 && symName.right(6)=="tilde;")
  {
    *letter=symName.at(1);
    return DocSymbol::Tilde;
  }
  else if (l==8 && symName.right(6)=="cedil;")
  {
    *letter=symName.at(1);
    return DocSymbol::Cedil;
  }
  else if (l==7 && symName.right(5)=="ring;")
  {
    *letter=symName.at(1);
    return DocSymbol::Ring;
  }
  else if (l==8 && symName.right(6)=="slash;")
  {
    *letter=symName.at(1);
    return DocSymbol::Slash;
  }
  return DocSymbol::Unknown;
}

//---------------------------------------------------------------------------

static int internalValidatingParseDoc(DocNode *parent,QList<DocNode> &children,
                                    const QString &doc)
{
  int retval = RetVal_OK;

  if (doc.isEmpty()) return retval;

  doctokenizerYYinit(doc,g_fileName);

  // first parse any number of paragraphs
  bool isFirst=TRUE;
  DocPara *lastPar=0;
  if (!children.isEmpty() && children.last()->kind()==DocNode::Kind_Para)
  { // last child item was a paragraph
    lastPar = (DocPara*)children.last();
    isFirst=FALSE;
  }
  do
  {
    DocPara *par = new DocPara(parent);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    retval=par->parse();
    if (!par->isEmpty()) 
    {
      children.append(par);
      if (lastPar) lastPar->markLast(FALSE);
      lastPar=par;
    }
    else
    {
      delete par;
    }
  } while (retval==TK_NEWPARA);
  if (lastPar) lastPar->markLast();

  return retval;
}

//---------------------------------------------------------------------------

static void readTextFileByName(const QString &file,QString &text)
{
  bool ambig;
  FileDef *fd;
  if ((fd=findFileDef(Doxygen::exampleNameDict,file,ambig)))
  {
    text = fileToString(fd->absFilePath(),FALSE);
  }
  else if (ambig)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: included file name %s is ambigious"
           "Possible candidates:\n%s",file.data(),
           showFileDefMatches(Doxygen::exampleNameDict,file).data()
          );
  }
  else
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: included file %s is not found. "
           "Check your EXAMPLE_PATH",file.data());
  }
}

//---------------------------------------------------------------------------

DocWord::DocWord(DocNode *parent,const QString &word) : 
      m_parent(parent), m_word(word) 
{
  //printf("new word %s url=%s\n",word.data(),g_searchUrl.data());
  if (!g_searchUrl.isEmpty())
  {
    Doxygen::searchIndex->addWord(word,FALSE);
  }
}

//---------------------------------------------------------------------------

DocLinkedWord::DocLinkedWord(DocNode *parent,const QString &word,
                  const QString &ref,const QString &file,
                  const QString &anchor) : 
      m_parent(parent), m_word(word), m_ref(ref), 
      m_file(file), m_relPath(g_relPath), m_anchor(anchor) 
{
  //printf("new word %s url=%s\n",word.data(),g_searchUrl.data());
  if (!g_searchUrl.isEmpty())
  {
    Doxygen::searchIndex->addWord(word,FALSE);
  }
}

//---------------------------------------------------------------------------

DocAnchor::DocAnchor(DocNode *parent,const QString &id,bool newAnchor) 
  : m_parent(parent)
{
  if (id.isEmpty())
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Empty anchor label");
  }
  if (newAnchor) // found <a name="label">
  {
    m_anchor = id;
  }
  else // found \anchor label
  {
    SectionInfo *sec = Doxygen::sectionDict[id];
    if (sec)
    {
      //printf("Found anchor %s\n",id.data());
      m_file   = sec->fileName;
      m_anchor = sec->label;
      if (g_sectionDict && g_sectionDict->find(id)==0)
      {
        //printf("Inserting in dictionary!\n");
        g_sectionDict->insert(id,sec);
      }
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Invalid anchor id `%s'",id.data());
      m_anchor = "invalid";
      m_file = "invalid";
    }
  }
}

//---------------------------------------------------------------------------

DocVerbatim::DocVerbatim(DocNode *parent,const QString &context,
    const QString &text, Type t,bool isExample,
    const QString &exampleFile) 
  : m_parent(parent), m_context(context), m_text(text), m_type(t),
    m_isExample(isExample), m_exampleFile(exampleFile), m_relPath(g_relPath) 
{
}


//---------------------------------------------------------------------------

void DocInclude::parse()
{
  DBG(("DocInclude::parse(file=%s,text=%s)\n",m_file.data(),m_text.data()));
  switch(m_type)
  {
    case IncWithLines:
      // fall through
    case Include:
      // fall through
    case DontInclude:
      readTextFileByName(m_file,m_text);
      g_includeFileText   = m_text;
      g_includeFileOffset = 0;
      g_includeFileLength = m_text.length();
      //printf("g_includeFile=<<%s>>\n",g_includeFileText.data());
      break;
    case VerbInclude: 
      // fall through
    case HtmlInclude:
      readTextFileByName(m_file,m_text);
      break;
  }
}

//---------------------------------------------------------------------------

void DocIncOperator::parse()
{
  const char *p = g_includeFileText;
  uint l = g_includeFileLength;
  uint o = g_includeFileOffset;
  DBG(("DocIncOperator::parse() text=%s off=%d len=%d\n",p,o,l));
  uint so = o,bo;
  bool nonEmpty = FALSE;
  switch(type())
  {
    case Line:
      while (o<l)
      {
        char c = p[o];
        if (c=='\n') 
        {
          if (nonEmpty) break; // we have a pattern to match
          so=o+1; // no pattern, skip empty line
        }
        else if (!isspace((uchar)c)) // no white space char
        {
          nonEmpty=TRUE;
        }
        o++;
      }
      if (g_includeFileText.mid(so,o-so).find(m_pattern)!=-1)
      {
        m_text = g_includeFileText.mid(so,o-so);
        DBG(("DocIncOperator::parse() Line: %s\n",m_text.data()));
      }
      g_includeFileOffset = QMIN(l,o+1); // set pointer to start of new line
      break;
    case SkipLine:
      while (o<l)
      {
        so=o;
        while (o<l)
        {
          char c = p[o];
          if (c=='\n')
          {
            if (nonEmpty) break; // we have a pattern to match
            so=o+1; // no pattern, skip empty line
          }
          else if (!isspace((uchar)c)) // no white space char
          {
            nonEmpty=TRUE;
          }
          o++;
        }
        if (g_includeFileText.mid(so,o-so).find(m_pattern)!=-1)
        {
          m_text = g_includeFileText.mid(so,o-so);
          DBG(("DocIncOperator::parse() SkipLine: %s\n",m_text.data()));
          break;
        }
        o++; // skip new line
      }
      g_includeFileOffset = QMIN(l,o+1); // set pointer to start of new line
      break;
    case Skip:
      while (o<l)
      {
        so=o;
        while (o<l)
        {
          char c = p[o];
          if (c=='\n')
          {
            if (nonEmpty) break; // we have a pattern to match
            so=o+1; // no pattern, skip empty line
          }
          else if (!isspace((uchar)c)) // no white space char
          {
            nonEmpty=TRUE;
          }
          o++;
        }
        if (g_includeFileText.mid(so,o-so).find(m_pattern)!=-1)
        {
          break;
        }
        o++; // skip new line
      }
      g_includeFileOffset = so; // set pointer to start of new line
      break;
    case Until:
      bo=o;
      while (o<l)
      {
        so=o;
        while (o<l)
        {
          char c = p[o];
          if (c=='\n')
          {
            if (nonEmpty) break; // we have a pattern to match
            so=o+1; // no pattern, skip empty line
          }
          else if (!isspace((uchar)c)) // no white space char
          {
            nonEmpty=TRUE;
          }
          o++;
        }
        if (g_includeFileText.mid(so,o-so).find(m_pattern)!=-1)
        {
          m_text = g_includeFileText.mid(bo,o-bo);
          DBG(("DocIncOperator::parse() Until: %s\n",m_text.data()));
          break;
        }
        o++; // skip new line
      }
      g_includeFileOffset = QMIN(l,o+1); // set pointer to start of new line
      break;
  }
}

//---------------------------------------------------------------------------

void DocCopy::parse()
{
  QString doc,brief;
  Definition *def;
  if (findDocsForMemberOrCompound(m_link,&doc,&brief,&def))
  {
    if (g_copyStack.findRef(def)==-1) // definition not parsed earlier
    {
      docParserPushContext();
      if (def->definitionType()==Definition::TypeMember && def->getOuterScope())
      {
        g_context=def->getOuterScope()->name();
      }
      else
      {
        g_context=def->name();
      }
      g_styleStack.clear();
      g_nodeStack.clear();
      g_copyStack.append(def);
      internalValidatingParseDoc(this,m_children,brief);
      internalValidatingParseDoc(this,m_children,doc);
      g_copyStack.remove(def);
      ASSERT(g_styleStack.isEmpty());
      ASSERT(g_nodeStack.isEmpty());
      docParserPopContext();
    }
    else // oops, recursion
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: recursive call chain of \\copydoc commands detected at %d\n",
          doctokenizerYYlineno);
    }
  }
  else
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: target %s of \\copydoc command not found",
        m_link.data());
  }
}

//---------------------------------------------------------------------------

DocXRefItem::DocXRefItem(DocNode *parent,int id,const char *key) : 
   m_parent(parent), m_id(id), m_key(key), m_relPath(g_relPath)
{
}

bool DocXRefItem::parse()
{
  QString listName;
  RefList *refList = Doxygen::xrefLists->find(m_key); 
  if (refList && 
      (
       // either not a built-in list or the list is enabled
       (m_key!="todo"       || Config_getBool("GENERATE_TODOLIST")) && 
       (m_key!="test"       || Config_getBool("GENERATE_TESTLIST")) && 
       (m_key!="bug"        || Config_getBool("GENERATE_BUGLIST"))  && 
       (m_key!="deprecated" || Config_getBool("GENERATE_DEPRECATEDLIST"))
      ) 
     )
  {
    RefItem *item = refList->getRefItem(m_id);
    ASSERT(item!=0);
    if (item)
    {
      if (g_memberDef && g_memberDef->name().at(0)=='@')
      {
        m_file   = "@";  // can't cross reference anonymous enum
        m_anchor = "@";
      }
      else
      {
        m_file   = refList->listName();
        m_anchor = item->listAnchor;
      }
      m_title  = refList->sectionTitle();
      //printf("DocXRefItem: file=%s anchor=%s title=%s\n",
      //    m_file.data(),m_anchor.data(),m_title.data());

      if (!item->text.isEmpty())
      {
        docParserPushContext();
        internalValidatingParseDoc(this,m_children,item->text);
        docParserPopContext();
      }
    }
    return TRUE;
  }
  return FALSE;
}

//---------------------------------------------------------------------------

DocFormula::DocFormula(DocNode *parent,int id) :
      m_parent(parent), m_relPath(g_relPath)
{
  QString formCmd;
  formCmd.sprintf("\\form#%d",id);
  Formula *formula=Doxygen::formulaNameDict[formCmd];
  if (formula)
  {
    m_id = formula->getId();
    m_name.sprintf("form_%d",m_id);
    m_text = formula->getFormulaText();
  }
}

//---------------------------------------------------------------------------

//int DocLanguage::parse()
//{
//  int retval;
//  DBG(("DocLanguage::parse() start\n"));
//  g_nodeStack.push(this);
//
//  // parse one or more paragraphs
//  bool isFirst=TRUE;
//  DocPara *par=0;
//  do
//  {
//    par = new DocPara(this);
//    if (isFirst) { par->markFirst(); isFirst=FALSE; }
//    m_children.append(par);
//    retval=par->parse();
//  }
//  while (retval==TK_NEWPARA);
//  if (par) par->markLast();
//
//  DBG(("DocLanguage::parse() end\n"));
//  DocNode *n = g_nodeStack.pop();
//  ASSERT(n==this);
//  return retval;
//}

//---------------------------------------------------------------------------

void DocSecRefItem::parse()
{
  DBG(("DocSecRefItem::parse() start\n"));
  g_nodeStack.push(this);

  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\refitem",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
	       tokToString(tok));
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();
  handlePendingStyleCommands(this,m_children);

  SectionInfo *sec=0;
  if (!m_target.isEmpty())
  {
    sec=Doxygen::sectionDict[m_target];
    if (sec)
    {
      m_file   = sec->fileName;
      m_anchor = sec->label;
      if (g_sectionDict && g_sectionDict->find(m_target)==0)
      {
        g_sectionDict->insert(m_target,sec);
      }
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning reference to unknown section %s",
          m_target.data());
    }
  } 
  else
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning reference to empty target");
  }
  
  DBG(("DocSecRefItem::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

void DocSecRefList::parse()
{
  DBG(("DocSecRefList::parse() start\n"));
  g_nodeStack.push(this);

  int tok=doctokenizerYYlex();
  // skip white space
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // handle items
  while (tok)
  {
    if (tok==TK_COMMAND)
    {
      switch (Mappers::cmdMapper->map(g_token->name))
      {
        case CMD_SECREFITEM:
          {
            int tok=doctokenizerYYlex();
            if (tok!=TK_WHITESPACE)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after \\refitem command");
              break;
            }
            tok=doctokenizerYYlex();
            if (tok!=TK_WORD && tok!=TK_LNKWORD)
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of \\refitem",
                  tokToString(tok));
              break;
            }

            DocSecRefItem *item = new DocSecRefItem(this,g_token->name);
            m_children.append(item);
            item->parse();
          }
          break;
        case CMD_ENDSECREFLIST:
          goto endsecreflist;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\secreflist",
              g_token->name.data());
          goto endsecreflist;
      }
    }
    else if (tok==TK_WHITESPACE)
    {
      // ignore whitespace
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s inside section reference list",
          tokToString(tok));
      goto endsecreflist;
    }
    tok=doctokenizerYYlex();
  }

endsecreflist:
  DBG(("DocSecRefList::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

DocInternalRef::DocInternalRef(DocNode *parent,const QString &ref) 
  : m_parent(parent), m_relPath(g_relPath)
{
  int i=ref.find('#');
  if (i!=-1)
  {
    m_anchor = ref.right(ref.length()-i-1);
    m_file   = ref.left(i);
  }
  else
  {
    m_file = ref;
  }
}

void DocInternalRef::parse()
{
  g_nodeStack.push(this);
  DBG(("DocInternalRef::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\ref",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok));
          break;
      }
    }
  }

  handlePendingStyleCommands(this,m_children);
  DBG(("DocInternalRef::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

DocRef::DocRef(DocNode *parent,const QString &target) : 
   m_parent(parent), m_refToSection(FALSE), m_refToAnchor(FALSE)
{
  Definition  *compound = 0;
  QCString     anchor;
  ASSERT(!target.isEmpty());
  m_relPath = g_relPath;
  SectionInfo *sec = Doxygen::sectionDict[target];
  if (sec) // ref to section or anchor
  {
    m_text         = sec->title;
    if (m_text.isEmpty()) m_text = sec->label;

    m_ref          = sec->ref;
    m_file         = stripKnownExtensions(sec->fileName);
    if (sec->type!=SectionInfo::Page) m_anchor = sec->label;
    m_refToAnchor  = sec->type==SectionInfo::Anchor;
    m_refToSection = sec->type!=SectionInfo::Anchor;
    //printf("m_text=%s,m_ref=%s,m_file=%s,m_refToAnchor=%d\n",
    //    m_text.data(),m_ref.data(),m_file.data(),m_refToAnchor);
    return;
  }
  else if (resolveLink(g_context,target,TRUE,&compound,anchor))
  {
    bool isFile = compound ? 
                 (compound->definitionType()==Definition::TypeFile ? TRUE : FALSE) : 
                 FALSE;
    m_text = linkToText(target,isFile);
    m_anchor = anchor;
    if (compound && compound->isLinkable()) // ref to compound
    {
      if (anchor.isEmpty() &&                                  /* compound link */
          compound->definitionType()==Definition::TypeGroup && /* is group */
          ((GroupDef *)compound)->groupTitle()                 /* with title */
         )
      {
        m_text=((GroupDef *)compound)->groupTitle(); // use group's title as link
      }
      else if (compound->definitionType()==Definition::TypeMember &&
          ((MemberDef*)compound)->isObjCMethod())
      {
        // Objective C Method
        MemberDef *member = (MemberDef*)compound;
        bool localLink = g_memberDef ? member->getClassDef()==g_memberDef->getClassDef() : FALSE;
        m_text = member->objCMethodName(localLink,g_inSeeBlock);
      }

      m_file = compound->getOutputFileBase();
      m_ref  = compound->getReference();
      return;
    }
    else if (compound->definitionType()==Definition::TypeFile && 
             ((FileDef*)compound)->generateSourceFile()
            ) // undocumented file that has source code we can link to
    {
      m_file = compound->getSourceFileBase();
      m_ref  = compound->getReference();
      return;
    }
  }
  m_text = linkToText(target,FALSE);
  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unable to resolve reference to `%s' for \\ref command",
           target.data()); 
}

void DocRef::parse()
{
  g_nodeStack.push(this);
  DBG(("DocRef::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\ref",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok));
          break;
      }
    }
  }

  handlePendingStyleCommands(this,m_children);
  DBG(("DocRef::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

DocLink::DocLink(DocNode *parent,const QString &target) : 
      m_parent(parent)
{
  Definition *compound;
  //PageInfo *page;
  QCString anchor;
  m_refText = target;
  m_relPath = g_relPath;
  if (!m_refText.isEmpty() && m_refText.at(0)=='#')
  {
    m_refText = m_refText.right(m_refText.length()-1);
  }
  if (resolveLink(g_context,stripKnownExtensions(target),g_inSeeBlock,
                  &compound,anchor))
  {
    m_anchor = anchor;
    if (compound && compound->isLinkable())
    {
      m_file = compound->getOutputFileBase();
      m_ref  = compound->getReference();
    }
    else if (compound->definitionType()==Definition::TypeFile && 
             ((FileDef*)compound)->generateSourceFile()
            ) // undocumented file that has source code we can link to
    {
      m_file = compound->getSourceFileBase();
      m_ref  = compound->getReference();
    }
    return;
  }

  // bogus link target
  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unable to resolve link to `%s' for \\link command",
         target.data()); 
}


QString DocLink::parse(bool isJavaLink)
{
  QString result;
  g_nodeStack.push(this);
  DBG(("DocLink::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children,FALSE))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          switch (Mappers::cmdMapper->map(g_token->name))
          {
            case CMD_ENDLINK:
              if (isJavaLink)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: {@link.. ended with @endlink command");
              }
              goto endlink;
            default:
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\link",
                  g_token->name.data());
              break;
          }
          break;
        case TK_SYMBOL: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
              g_token->name.data());
          break;
        case TK_LNKWORD: 
        case TK_WORD: 
          if (isJavaLink) // special case to detect closing }
          {
            QString w = g_token->name;
            int p;
            if (w=="}")
            {
              goto endlink;
            }
            else if ((p=w.find('}'))!=-1)
            {
              uint l=w.length();
              m_children.append(new DocWord(this,w.left(p)));
              if ((uint)p<l-1) // something left after the } (for instance a .)
              {
                result=w.right(l-p-1);
              }
              goto endlink;
            }
          }
          m_children.append(new DocWord(this,g_token->name));
          break;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
             tokToString(tok));
        break;
      }
    }
  }
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected end of comment while inside"
           " link command\n"); 
  }
endlink:

  if (m_children.isEmpty()) // no link text
  {
    m_children.append(new DocWord(this,m_refText));
  }

  handlePendingStyleCommands(this,m_children);
  DBG(("DocLink::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return result;
}


//---------------------------------------------------------------------------

DocDotFile::DocDotFile(DocNode *parent,const QString &name) : 
      m_parent(parent), m_name(name), m_relPath(g_relPath)
{
}

void DocDotFile::parse()
{
  g_nodeStack.push(this);
  DBG(("DocDotFile::parse() start\n"));

  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\dotfile",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok));
          break;
      }
    }
  }
  tok=doctokenizerYYlex();
  while (tok==TK_WORD) // there are values following the title
  {
    if (g_token->name=="width") 
    {
      m_width=g_token->chars;
    }
    else if (g_token->name=="height") 
    {
      m_height=g_token->chars;
    }
    else 
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unknown option %s after image title",
            g_token->name.data());
    }
    tok=doctokenizerYYlex();
  }
  ASSERT(tok==0);
  doctokenizerYYsetStatePara();
  handlePendingStyleCommands(this,m_children);

  bool ambig;
  FileDef *fd = findFileDef(Doxygen::dotFileNameDict,m_name,ambig);
  if (fd)
  {
    m_file = fd->absFilePath();
  }
  else if (ambig)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: included dot file name %s is ambigious.\n"
           "Possible candidates:\n%s",m_name.data(),
           showFileDefMatches(Doxygen::exampleNameDict,m_name).data()
          );
  }
  else
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: included dot file %s is not found "
           "in any of the paths specified via DOTFILE_DIRS!",m_name.data());
  }

  DBG(("DocDotFile::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}


//---------------------------------------------------------------------------

DocImage::DocImage(DocNode *parent,const HtmlAttribList &attribs,const QString &name,Type t) : 
      m_parent(parent), m_attribs(attribs), m_name(name), 
      m_type(t), m_relPath(g_relPath)
{
}

void DocImage::parse()
{
  g_nodeStack.push(this);
  DBG(("DocImage::parse() start\n"));

  // parse title
  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (tok==TK_WORD && (g_token->name=="width=" || g_token->name=="height="))
    {
      // special case: no title, but we do have a size indicator
      doctokenizerYYsetStateTitleAttrValue();
      // strip =
      g_token->name=g_token->name.left(g_token->name.length()-1);
      break;
    } 
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a \\image",
              g_token->name.data());
          break;
        case TK_SYMBOL: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
              g_token->name.data());
          break;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
              tokToString(tok));
          break;
      }
    }
  }
  // parse size attributes
  tok=doctokenizerYYlex();
  while (tok==TK_WORD) // there are values following the title
  {
    if (g_token->name=="width") 
    {
      m_width=g_token->chars;
    }
    else if (g_token->name=="height") 
    {
      m_height=g_token->chars;
    }
    else 
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unknown option %s after image title",
          g_token->name.data());
    }
    tok=doctokenizerYYlex();
  }
  doctokenizerYYsetStatePara();

  handlePendingStyleCommands(this,m_children);
  DBG(("DocImage::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}


//---------------------------------------------------------------------------

int DocHtmlHeader::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlHeader::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a <h%d> tag",
	       g_token->name.data(),m_level);
          break;
        case TK_HTMLTAG:
          {
            int tagId=Mappers::htmlTagMapper->map(g_token->name);
            if (tagId==HTML_H1 && g_token->endTag) // found </h1> tag
            {
              if (m_level!=1)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h1>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H2 && g_token->endTag) // found </h2> tag
            {
              if (m_level!=2)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h2>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H3 && g_token->endTag) // found </h3> tag
            {
              if (m_level!=3)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h3>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H4 && g_token->endTag) // found </h4> tag
            {
              if (m_level!=4)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h4>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H5 && g_token->endTag) // found </h5> tag
            {
              if (m_level!=5)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h5>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H6 && g_token->endTag) // found </h6> tag
            {
              if (m_level!=6)
              {
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: <h%d> ended with </h6>",
                    m_level); 
              }
              goto endheader;
            }
            else if (tagId==HTML_A)
            {
              if (!g_token->endTag)
              {
                handleAHref(this,m_children,g_token->attribs);
              }
            }
            else if (tagId==HTML_BR)
            {
              DocLineBreak *lb = new DocLineBreak(this);
              m_children.append(lb);
            }
            else
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected html tag <%s%s> found within <h%d> context",
                  g_token->endTag?"/":"",g_token->name.data(),m_level);
            }
            
          }
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok));
          break;
      }
    }
  }
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected end of comment while inside"
           " <h%d> tag\n",m_level); 
  }
endheader:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlHeader::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHRef::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHRef::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a <a>..</a> block",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        case TK_HTMLTAG:
          {
            int tagId=Mappers::htmlTagMapper->map(g_token->name);
            if (tagId==HTML_A && g_token->endTag) // found </a> tag
            {
              goto endhref;
            }
            else
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected html tag <%s%s> found within <a href=...> context",
                  g_token->endTag?"/":"",g_token->name.data(),doctokenizerYYlineno);
            }
          }
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected end of comment while inside"
           " <a href=...> tag",doctokenizerYYlineno); 
  }
endhref:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHRef::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocInternal::parse(int level)
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocInternal::parse() start\n"));

  // first parse any number of paragraphs
  bool isFirst=TRUE;
  DocPara *lastPar=0;
  do
  {
    DocPara *par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    retval=par->parse();
    if (!par->isEmpty()) 
    {
      m_children.append(par);
      lastPar=par;
    }
    else
    {
      delete par;
    }
    if (retval==TK_LISTITEM)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Invalid list item found",doctokenizerYYlineno);
    }
  } while (retval!=0 && 
           retval!=RetVal_Section &&
           retval!=RetVal_Subsection &&
           retval!=RetVal_Subsubsection &&
           retval!=RetVal_Paragraph
          );
  if (lastPar) lastPar->markLast();

  // then parse any number of level-n sections
  while ((level==1 && retval==RetVal_Section) || 
         (level==2 && retval==RetVal_Subsection) ||
         (level==3 && retval==RetVal_Subsubsection) ||
         (level==4 && retval==RetVal_Paragraph)
        )
  {
    DocSection *s=new DocSection(this,level,g_token->sectionId);
    m_children.append(s);
    retval = s->parse();
  }

  if (retval==RetVal_Internal)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: \\internal command found inside internal section");
  }

  DBG(("DocInternal::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocIndexEntry::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocIndexEntry::parse() start\n"));
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after \\addindex command");
    goto endindexentry;
  }
  doctokenizerYYsetStateTitle();
  m_entry="";
  while ((tok=doctokenizerYYlex()))
  {
    switch (tok)
    {
      case TK_WHITESPACE:
        m_entry+=" ";
        break;
      case TK_WORD: 
      case TK_LNKWORD: 
        m_entry+=g_token->name;
        break;
      case TK_SYMBOL:
        {
          char letter='\0';
          DocSymbol::SymType s = DocSymbol::decodeSymbol(g_token->name,&letter);
          switch (s)
          {
            case DocSymbol::BSlash:  m_entry+='\\'; break;
            case DocSymbol::At:      m_entry+='@';  break;
            case DocSymbol::Less:    m_entry+='<';  break;
            case DocSymbol::Greater: m_entry+='>';  break;
            case DocSymbol::Amp:     m_entry+='&';  break;
            case DocSymbol::Dollar:  m_entry+='$';  break;
            case DocSymbol::Hash:    m_entry+='#';  break;
            case DocSymbol::Percent: m_entry+='%';  break;
            case DocSymbol::Apos:    m_entry+='\''; break;
            case DocSymbol::Quot:    m_entry+='"';  break;
            case DocSymbol::Lsquo:   m_entry+='`';  break;
            case DocSymbol::Rsquo:   m_entry+='\'';  break;
            case DocSymbol::Ldquo:   m_entry+="``";  break;
            case DocSymbol::Rdquo:   m_entry+="''";  break;
            case DocSymbol::Ndash:   m_entry+="--";  break;
            case DocSymbol::Mdash:   m_entry+="---";  break;
            default:
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected symbol found as argument of \\addindex");
              break;
          }
        }
        break;
    case TK_COMMAND: 
      switch (Mappers::cmdMapper->map(g_token->name))
      {
        case CMD_BSLASH:  m_entry+='\\'; break;
        case CMD_AT:      m_entry+='@';  break;
        case CMD_LESS:    m_entry+='<';  break;
        case CMD_GREATER: m_entry+='>';  break;
        case CMD_AMP:     m_entry+='&';  break;
        case CMD_DOLLAR:  m_entry+='$';  break;
        case CMD_HASH:    m_entry+='#';  break;
        case CMD_PERCENT: m_entry+='%';  break;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected command %s found as argument of \\addindex",
                    g_token->name.data());
          break;
      }
      break;
      default:
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
            tokToString(tok));
        break;
    }
  }
  if (tok!=0) retval=tok;
  doctokenizerYYsetStatePara();
endindexentry:
  DBG(("DocIndexEntry::parse() end retval=%x\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlCaption::parse()
{
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlCaption::parse() start\n"));
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a <caption> tag",
              g_token->name.data());
          break;
        case TK_SYMBOL: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
              g_token->name.data());
          break;
        case TK_HTMLTAG:
          {
            int tagId=Mappers::htmlTagMapper->map(g_token->name);
            if (tagId==HTML_CAPTION && g_token->endTag) // found </caption> tag
            {
              retval = RetVal_OK;
              goto endcaption;
            }
            else
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected html tag <%s%s> found within <caption> context",
                  g_token->endTag?"/":"",g_token->name.data());
            }
          }
          break;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
              tokToString(tok));
          break;
      }
    }
  }
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected end of comment while inside"
           " <caption> tag",doctokenizerYYlineno); 
  }
endcaption:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlCaption::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlCell::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlCell::parse() start\n"));

  // parse one or more paragraphs
  bool isFirst=TRUE;
  DocPara *par=0;
  do
  {
    par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    m_children.append(par);
    retval=par->parse();
    if (retval==TK_HTMLTAG)
    {
      int tagId=Mappers::htmlTagMapper->map(g_token->name);
      if (tagId==HTML_TD && g_token->endTag) // found </dt> tag
      {
        retval=TK_NEWPARA; // ignore the tag
      }
      else if (tagId==HTML_TH && g_token->endTag) // found </th> tag
      {
        retval=TK_NEWPARA; // ignore the tag
      }
    }
  }
  while (retval==TK_NEWPARA);
  if (par) par->markLast();

  DBG(("DocHtmlCell::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlRow::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlRow::parse() start\n"));

  bool isHeading=FALSE;
  bool isFirst=TRUE;
  DocHtmlCell *cell=0;

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=Mappers::htmlTagMapper->map(g_token->name);
    if (tagId==HTML_TD && !g_token->endTag) // found <td> tag
    {
    }
    else if (tagId==HTML_TH && !g_token->endTag) // found <th> tag
    {
      isHeading=TRUE;
    }
    else // found some other tag
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <td> or <th> tag but "
          "found <%s> instead!",g_token->name.data());
      goto endrow;
    }
  }
  else if (tok==0) // premature end of comment
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while looking"
        " for a html description title");
    goto endrow;
  }
  else // token other than html token
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <td> or <th> tag but found %s token instead!",
        tokToString(tok));
    goto endrow;
  }

  // parse one or more cells
  do
  {
    cell=new DocHtmlCell(this,g_token->attribs,isHeading);
    cell->markFirst(isFirst);
    isFirst=FALSE;
    m_children.append(cell);
    retval=cell->parse();
    isHeading = retval==RetVal_TableHCell;
  }
  while (retval==RetVal_TableCell || retval==RetVal_TableHCell);
  if (cell) cell->markLast(TRUE);

endrow:
  DBG(("DocHtmlRow::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlTable::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlTable::parse() start\n"));
  
getrow:
  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=Mappers::htmlTagMapper->map(g_token->name);
    if (tagId==HTML_TR && !g_token->endTag) // found <tr> tag
    {
      // no caption, just rows
      retval=RetVal_TableRow;
    }
    else if (tagId==HTML_CAPTION && !g_token->endTag) // found <caption> tag
    {
      if (m_caption)
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: table already has a caption, found another one");
      }
      else
      {
        m_caption = new DocHtmlCaption(this,g_token->attribs);
        retval=m_caption->parse();

        if (retval==RetVal_OK) // caption was parsed ok
        {
          goto getrow;
        }
      }
    }
    else // found wrong token
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <tr> or <caption> tag but "
          "found <%s%s> instead!", g_token->endTag ? "/" : "", g_token->name.data());
    }
  }
  else if (tok==0) // premature end of comment
  {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while looking"
          " for a <tr> or <caption> tag");
  }
  else // token other than html token
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <tr> tag but found %s token instead!",
        tokToString(tok));
  }
       
  // parse one or more rows
  while (retval==RetVal_TableRow)
  {
    DocHtmlRow *tr=new DocHtmlRow(this,g_token->attribs);
    m_children.append(tr);
    retval=tr->parse();
  } 

  DBG(("DocHtmlTable::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval==RetVal_EndTable ? RetVal_OK : retval;
}

uint DocHtmlTable::numCols() const
{
  uint cols=0;
  QListIterator<DocNode> cli(m_children);
  DocNode *n;
  for (cli.toFirst();(n=cli.current());++cli)
  {
    ASSERT(n->kind()==DocNode::Kind_HtmlRow);
    cols=QMAX(cols,((DocHtmlRow *)n)->numCells());
  }
  return cols;
}

void DocHtmlTable::accept(DocVisitor *v) 
{ 
  v->visitPre(this); 
  // for HTML output we put the caption first
  if (m_caption && v->id()==DocVisitor_Html) m_caption->accept(v);
  QListIterator<DocNode> cli(m_children);
  DocNode *n;
  for (cli.toFirst();(n=cli.current());++cli) n->accept(v);
  // for other output formats we put the caption last
  if (m_caption && v->id()!=DocVisitor_Html) m_caption->accept(v);
  v->visitPost(this); 
}

//---------------------------------------------------------------------------

int DocHtmlDescTitle::parse()
{
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescTitle::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          {
            QString cmdName=g_token->name;
            bool isJavaLink=FALSE;
            switch (Mappers::cmdMapper->map(cmdName))
            {
              case CMD_REF:
                {
                  int tok=doctokenizerYYlex();
                  if (tok!=TK_WHITESPACE)
                  {
                    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
                        g_token->name.data());
                  }
                  else
                  {
                    doctokenizerYYsetStateRef();
                    tok=doctokenizerYYlex(); // get the reference id
                    if (tok!=TK_WORD)
                    {
                      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
                          tokToString(tok),cmdName.data());
                    }
                    else
                    {
                      DocRef *ref = new DocRef(this,g_token->name);
                      m_children.append(ref);
                      ref->parse();
                    }
                    doctokenizerYYsetStatePara();
                  }
                }
                break;
              case CMD_JAVALINK:
                isJavaLink=TRUE;
                // fall through
              case CMD_LINK:
                {
                  int tok=doctokenizerYYlex();
                  if (tok!=TK_WHITESPACE)
                  {
                    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
                        cmdName.data());
                  }
                  else
                  {
                    doctokenizerYYsetStateLink();
                    tok=doctokenizerYYlex();
                    if (tok!=TK_WORD)
                    {
                      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
                          tokToString(tok),cmdName.data());
                    }
                    else
                    {
                      doctokenizerYYsetStatePara();
                      DocLink *lnk = new DocLink(this,g_token->name);
                      m_children.append(lnk);
                      QString leftOver = lnk->parse(isJavaLink);
                      if (!leftOver.isEmpty())
                      {
                        m_children.append(new DocWord(this,leftOver));
                      }
                    }
                  }
                }

                break;
              default:
                warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a <dt> tag",
                               g_token->name.data());
            }
          }
          break;
        case TK_SYMBOL: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
              g_token->name.data());
          break;
        case TK_HTMLTAG:
          {
            int tagId=Mappers::htmlTagMapper->map(g_token->name);
            if (tagId==HTML_DD && !g_token->endTag) // found <dd> tag
            {
              retval = RetVal_DescData;
              goto endtitle;
            }
            else if (tagId==HTML_DT && g_token->endTag)
            {
              // ignore </dt> tag.
            }
            else if (tagId==HTML_DT)
            {
              // missing <dt> tag.
              retval = RetVal_DescTitle;
              goto endtitle;
            }
            else if (tagId==HTML_DL && g_token->endTag)
            {
              retval=RetVal_EndDesc;
              goto endtitle;
            }
            else
            {
              warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected html tag <%s%s> found within <dt> context",
                  g_token->endTag?"/":"",g_token->name.data());
            }
          }
          break;
        default:
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
              tokToString(tok));
          break;
      }
    }
  }
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected end of comment while inside"
        " <dt> tag"); 
  }
endtitle:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlDescTitle::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlDescData::parse()
{
  m_attribs = g_token->attribs;
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescData::parse() start\n"));

  bool isFirst=TRUE;
  DocPara *par=0;
  do
  {
    par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);
  if (par) par->markLast();
  
  DBG(("DocHtmlDescData::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlDescList::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescList::parse() start\n"));

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=Mappers::htmlTagMapper->map(g_token->name);
    if (tagId==HTML_DT && !g_token->endTag) // found <dt> tag
    {
      // continue
    }
    else // found some other tag
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <dt> tag but "
          "found <%s> instead!",g_token->name.data());
      goto enddesclist;
    }
  }
  else if (tok==0) // premature end of comment
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while looking"
        " for a html description title");
    goto enddesclist;
  }
  else // token other than html token
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <dt> tag but found %s token instead!",
        tokToString(tok));
    goto enddesclist;
  }

  do
  {
    DocHtmlDescTitle *dt=new DocHtmlDescTitle(this,g_token->attribs);
    m_children.append(dt);
    DocHtmlDescData *dd=new DocHtmlDescData(this);
    m_children.append(dd);
    retval=dt->parse();
    if (retval==RetVal_DescData)
    {
      retval=dd->parse();
    }
    else if (retval!=RetVal_DescTitle)
    {
      // error
      break;
    }
  } while (retval==RetVal_DescTitle);

  if (retval==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while inside <dl> block");
  }

enddesclist:

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocHtmlDescList::parse() end\n"));
  return retval==RetVal_EndDesc ? RetVal_OK : retval;
}

//---------------------------------------------------------------------------

int DocHtmlListItem::parse()
{
  DBG(("DocHtmlListItem::parse() start\n"));
  int retval=0;
  g_nodeStack.push(this);

  // parse one or more paragraphs
  bool isFirst=TRUE;
  DocPara *par=0;
  do
  {
    par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);
  if (par) par->markLast();

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocHtmlListItem::parse() end retval=%x\n",retval));
  return retval;
}

int DocHtmlListItem::parseXml()
{
  DBG(("DocHtmlListItem::parseXml() start\n"));
  int retval=0;
  g_nodeStack.push(this);

  // parse one or more paragraphs
  bool isFirst=TRUE;
  DocPara *par=0;
  do
  {
    par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    m_children.append(par);
    retval=par->parse();
    if (retval==0) break;

    //printf("new item: retval=%x g_token->name=%s g_token->endTag=%d\n",
    //    retval,g_token->name.data(),g_token->endTag);
    if (retval==RetVal_ListItem)
    {
      break;
    }
  }
  while (retval!=RetVal_CloseXml);

  if (par) par->markLast();

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocHtmlListItem::parseXml() end retval=%x\n",retval));
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlList::parse()
{
  DBG(("DocHtmlList::parse() start\n"));
  int retval=RetVal_OK;
  int num=1;
  g_nodeStack.push(this);

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace and paragraph breaks
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=Mappers::htmlTagMapper->map(g_token->name);
    if (tagId==HTML_LI && !g_token->endTag) // found <li> tag
    {
      // ok, we can go on.
    }
    else // found some other tag
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <li> tag but "
          "found <%s> instead!",g_token->name.data());
      goto endlist;
    }
  }
  else if (tok==0) // premature end of comment
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while looking"
        " for a html list item");
    goto endlist;
  }
  else // token other than html token
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <li> tag but found %s token instead!",
        tokToString(tok));
    goto endlist;
  }

  do
  {
    DocHtmlListItem *li=new DocHtmlListItem(this,g_token->attribs,num++);
    m_children.append(li);
    retval=li->parse();
  } while (retval==RetVal_ListItem);
  
  if (retval==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while inside <%cl> block",
        m_type==Unordered ? 'u' : 'o');
  }

endlist:
  DBG(("DocHtmlList::parse() end retval=%x\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval==RetVal_EndList ? RetVal_OK : retval;
}

int DocHtmlList::parseXml()
{
  DBG(("DocHtmlList::parseXml() start\n"));
  int retval=RetVal_OK;
  int num=1;
  g_nodeStack.push(this);

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace and paragraph breaks
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=Mappers::htmlTagMapper->map(g_token->name);
    //printf("g_token->name=%s g_token->endTag=%d\n",g_token->name.data(),g_token->endTag);
    if (tagId==XML_ITEM && !g_token->endTag) // found <item> tag
    {
      // ok, we can go on.
    }
    else // found some other tag
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <item> tag but "
          "found <%s> instead!",g_token->name.data());
      goto endlist;
    }
  }
  else if (tok==0) // premature end of comment
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while looking"
        " for a html list item");
    goto endlist;
  }
  else // token other than html token
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected <item> tag but found %s token instead!",
        tokToString(tok));
    goto endlist;
  }

  do
  {
    DocHtmlListItem *li=new DocHtmlListItem(this,g_token->attribs,num++);
    m_children.append(li);
    retval=li->parseXml();
    if (retval==0) break;
    //printf("retval=%x g_token->name=%s\n",retval,g_token->name.data());
  } while (retval==RetVal_ListItem);
  
  if (retval==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment while inside <list type=\"%s\"> block",
        m_type==Unordered ? "bullet" : "number");
  }

endlist:
  DBG(("DocHtmlList::parseXml() end retval=%x\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval==RetVal_EndList ? RetVal_OK : retval;
}

//---------------------------------------------------------------------------

int DocSimpleListItem::parse()
{
  g_nodeStack.push(this);
  int rv=m_paragraph->parse();
  m_paragraph->markFirst();
  m_paragraph->markLast();
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return rv;
}

//--------------------------------------------------------------------------

int DocSimpleList::parse()
{
  g_nodeStack.push(this);
  int rv;
  do
  {
    DocSimpleListItem *li=new DocSimpleListItem(this);
    m_children.append(li);
    rv=li->parse();
  } while (rv==RetVal_ListItem);
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

//--------------------------------------------------------------------------

int DocAutoListItem::parse()
{
  int retval = RetVal_OK;
  g_nodeStack.push(this);
  retval=m_paragraph->parse();
  m_paragraph->markFirst();
  m_paragraph->markLast();
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

int DocAutoList::parse()
{
  int retval = RetVal_OK;
  int num=1;
  g_nodeStack.push(this);
	  // first item or sub list => create new list
  do
  {
    DocAutoListItem *li = new DocAutoListItem(this,num++);
    m_children.append(li);
    retval=li->parse();
  } 
  while (retval==TK_LISTITEM &&              // new list item
         m_indent==g_token->indent &&        // at same indent level
	 m_isEnumList==g_token->isEnumList   // of the same kind
        );

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

void DocTitle::parse()
{
  DBG(("DocTitle::parse() start\n"));
  g_nodeStack.push(this);
  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal command %s as part of a title section",
	       g_token->name.data());
          break;
        case TK_SYMBOL: 
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
               g_token->name.data());
          break;
        default:
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
		tokToString(tok));
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();
  handlePendingStyleCommands(this,m_children);
  DBG(("DocTitle::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

void DocTitle::parseFromString(const QString &text)
{
  m_children.append(new DocWord(this,text));
}

//--------------------------------------------------------------------------

DocSimpleSect::DocSimpleSect(DocNode *parent,Type t) : 
     m_parent(parent), m_type(t)
{ 
  m_title=0; 
}

DocSimpleSect::~DocSimpleSect()
{ 
  delete m_title; 
}

void DocSimpleSect::accept(DocVisitor *v)
{
  v->visitPre(this);
  if (m_title) m_title->accept(v);
  QListIterator<DocNode> cli(m_children);
  DocNode *n;
  for (cli.toFirst();(n=cli.current());++cli) n->accept(v);
  v->visitPost(this);
}

int DocSimpleSect::parse(bool userTitle)
{
  DBG(("DocSimpleSect::parse() start\n"));
  g_nodeStack.push(this);

  // handle case for user defined title
  if (userTitle)
  {
    m_title = new DocTitle(this);
    m_title->parse();
  }
  
  // add new paragraph as child
  DocPara *par = new DocPara(this);
  if (m_children.isEmpty()) 
  {
    par->markFirst();
  }
  else
  {
    ASSERT(m_children.last()->kind()==DocNode::Kind_Para);
    ((DocPara *)m_children.last())->markLast(FALSE);
  }
  par->markLast();
  m_children.append(par);
  
  // parse the contents of the paragraph
  int retval = par->parse();

  DBG(("DocSimpleSect::parse() end retval=%d\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval; // 0==EOF, TK_NEWPARA, TK_LISTITEM, TK_ENDLIST, RetVal_SimpleSec
}

int DocSimpleSect::parseRcs()
{
  DBG(("DocSimpleSect::parseRcs() start\n"));
  g_nodeStack.push(this);

  m_title = new DocTitle(this);
  m_title->parseFromString(g_token->name);

  docParserPushContext();
  internalValidatingParseDoc(this,m_children,g_token->text);
  docParserPopContext();

  DBG(("DocSimpleSect::parseRcs()\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return RetVal_OK; 
}

int DocSimpleSect::parseXml()
{
  DBG(("DocSimpleSect::parse() start\n"));
  g_nodeStack.push(this);

  int retval = RetVal_OK;
  for (;;) 
  {
    // add new paragraph as child
    DocPara *par = new DocPara(this);
    if (m_children.isEmpty()) 
    {
      par->markFirst();
    }
    else
    {
      ASSERT(m_children.last()->kind()==DocNode::Kind_Para);
      ((DocPara *)m_children.last())->markLast(FALSE);
    }
    par->markLast();
    m_children.append(par);

    // parse the contents of the paragraph
    retval = par->parse();
    if (retval == 0) break;
    if (retval == RetVal_CloseXml) 
    {
      retval = RetVal_OK;
      break;
    }
  }
  
  DBG(("DocSimpleSect::parseXml() end retval=%d\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval; 
}

void DocSimpleSect::appendLinkWord(const QString &word)
{
  DocPara *p;
  if (m_children.isEmpty() || m_children.last()->kind()!=DocNode::Kind_Para)
  {
    p = new DocPara(this);
    m_children.append(p);
  }
  else
  {
    p = (DocPara *)m_children.last();
    
    // Comma-seperate <seealso> links.
    p->injectToken(TK_WORD,",");
    p->injectToken(TK_WHITESPACE," ");
  }
  
  g_inSeeBlock=TRUE;
  p->injectToken(TK_LNKWORD,word);
  g_inSeeBlock=FALSE;
}

//--------------------------------------------------------------------------

int DocParamList::parse(const QString &cmdName)
{
  int retval=RetVal_OK;
  DBG(("DocParamList::parse() start\n"));
  g_nodeStack.push(this);
  DocPara *par=0;

  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
  }
  doctokenizerYYsetStateParam();
  tok=doctokenizerYYlex();
  while (tok==TK_WORD) /* there is a parameter name */
  {
    if (m_type==DocParamSect::Param)
    {
      g_hasParamCommand=TRUE;
      checkArgumentName(g_token->name,TRUE);
    }
    else if (m_type==DocParamSect::RetVal)
    {
      g_hasReturnCommand=TRUE;
      checkArgumentName(g_token->name,FALSE);
    }
    //m_params.append(g_token->name);
    handleLinkedWord(this,m_params);
    tok=doctokenizerYYlex();
  }
  doctokenizerYYsetStatePara();
  if (tok==0) /* premature end of comment block */
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
        "argument of command %s",cmdName.data());
    retval=0;
    goto endparamlist;
  }
  ASSERT(tok==TK_WHITESPACE);

  par = new DocPara(this);
  m_paragraphs.append(par);
  retval = par->parse();
  par->markFirst();
  par->markLast();

endparamlist:
  DBG(("DocParamList::parse() end retval=%d\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

int DocParamList::parseXml(const QString &paramName)
{
  int retval=RetVal_OK;
  DBG(("DocParamList::parseXml() start\n"));
  g_nodeStack.push(this);

  g_token->name = paramName;
  if (m_type==DocParamSect::Param)
  {
    g_hasParamCommand=TRUE;
    checkArgumentName(g_token->name,TRUE);
  }
  else if (m_type==DocParamSect::RetVal)
  {
    g_hasReturnCommand=TRUE;
    checkArgumentName(g_token->name,FALSE);
  }
  
  handleLinkedWord(this,m_params);

  do
  {
    DocPara *par = new DocPara(this);
    retval = par->parse();
    if (par->isEmpty()) // avoid adding an empty paragraph for the whitespace
                        // after </para> and before </param>
    {
      delete par;
      break;
    }
    else // append the paragraph to the list
    {
      if (m_paragraphs.isEmpty())
      {
        par->markFirst();
      }
      else
      {
        m_paragraphs.last()->markLast(FALSE);
      }
      par->markLast();
      m_paragraphs.append(par);
    }

    if (retval == 0) break;

  } while (retval==RetVal_CloseXml && 
           Mappers::htmlTagMapper->map(g_token->name)!=XML_PARAM &&
           Mappers::htmlTagMapper->map(g_token->name)!=XML_EXCEPTION);
  

  if (retval==0) /* premature end of comment block */
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unterminated param or exception tag");
  }
  else
  {
    retval=RetVal_OK;
  }


  DBG(("DocParamList::parse() end retval=%d\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

int DocParamSect::parse(const QString &cmdName,bool xmlContext, Direction d)
{
  int retval=RetVal_OK;
  DBG(("DocParamSect::parse() start\n"));
  g_nodeStack.push(this);

  DocParamList *pl = new DocParamList(this,m_type,d);
  if (m_children.isEmpty())
  {
    pl->markFirst();
    pl->markLast();
  }
  else
  {
    ASSERT(m_children.last()->kind()==DocNode::Kind_ParamList);
    ((DocParamList *)m_children.last())->markLast(FALSE);
    pl->markLast();
  }
  m_children.append(pl);
  if (xmlContext)
  {
    retval = pl->parseXml(cmdName);
  }
  else
  {
    retval = pl->parse(cmdName);
  }
  
  DBG(("DocParamSect::parse() end retval=%d\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

int DocPara::handleSimpleSection(DocSimpleSect::Type t, bool xmlContext)
{
  DocSimpleSect *ss=0;
  if (!m_children.isEmpty() &&                           // previous element
      m_children.last()->kind()==Kind_SimpleSect &&      // was a simple sect
      ((DocSimpleSect *)m_children.last())->type()==t && // of same type
      t!=DocSimpleSect::User)                            // but not user defined
  {
    // append to previous section
    ss=(DocSimpleSect *)m_children.last();
  }
  else // start new section
  {
    ss=new DocSimpleSect(this,t);
    m_children.append(ss);
  }
  int rv = RetVal_OK;
  if (xmlContext)
  {
    return ss->parseXml();
  }
  else
  {
    rv = ss->parse(t==DocSimpleSect::User);
  }
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

int DocPara::handleParamSection(const QString &cmdName,
                                DocParamSect::Type t,
                                bool xmlContext=FALSE,
                                int direction=DocParamSect::Unspecified)
{
  DocParamSect *ps=0;
  if (!m_children.isEmpty() &&                        // previous element
      m_children.last()->kind()==Kind_ParamSect &&    // was a param sect
      ((DocParamSect *)m_children.last())->type()==t) // of same type
  {
    // append to previous section
    ps=(DocParamSect *)m_children.last();
  }
  else // start new section
  {
    ps=new DocParamSect(this,t);
    m_children.append(ps);
  }
  int rv=ps->parse(cmdName,xmlContext,(DocParamSect::Direction)direction);
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

int DocPara::handleXRefItem()
{
  int retval=doctokenizerYYlex();
  ASSERT(retval==TK_WHITESPACE);
  doctokenizerYYsetStateXRefItem();
  retval=doctokenizerYYlex();
  if (retval==RetVal_OK)
  {
    DocXRefItem *ref = new DocXRefItem(this,g_token->id,g_token->name);
    if (ref->parse())
    {
      m_children.append(ref);
    }
    else 
    {
      delete ref;
    }
  }
  doctokenizerYYsetStatePara();
  return retval;
}

void DocPara::handleIncludeOperator(const QString &cmdName,DocIncOperator::Type t)
{
  DBG(("handleIncludeOperator(%s)\n",cmdName.data()));
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  doctokenizerYYsetStatePattern();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
        "argument of command %s", cmdName.data());
    return;
  }
  else if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  DocIncOperator *op = new DocIncOperator(this,t,g_token->name,g_context,g_isExample,g_exampleName);
  DocNode *n1 = m_children.last();
  DocNode *n2 = n1!=0 ? m_children.prev() : 0;
  bool isFirst = n1==0 || // no last node
                 (n1->kind()!=DocNode::Kind_IncOperator && 
                  n1->kind()!=DocNode::Kind_WhiteSpace
                 ) || // last node is not operator or whitespace
                 (n1->kind()==DocNode::Kind_WhiteSpace && 
                  n2!=0 && n2->kind()!=DocNode::Kind_IncOperator
                 ); // previous not is not operator
  op->markFirst(isFirst);
  op->markLast(TRUE);
  if (n1!=0 && n1->kind()==DocNode::Kind_IncOperator)
  {
    ((DocIncOperator *)n1)->markLast(FALSE);
  }
  else if (n1!=0 && n1->kind()==DocNode::Kind_WhiteSpace &&
           n2!=0 && n2->kind()==DocNode::Kind_IncOperator
          )
  {
    ((DocIncOperator *)n2)->markLast(FALSE);
  }
  m_children.append(op);
  op->parse();
}

void DocPara::handleImage(const QString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD && tok!=TK_LNKWORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  DocImage::Type t;
  QString imgType = g_token->name.lower();
  if      (imgType=="html")  t=DocImage::Html;
  else if (imgType=="latex") t=DocImage::Latex;
  else if (imgType=="rtf")   t=DocImage::Rtf;
  else
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: image type %s specified as the first argument of "
        "%s is not valid",
        imgType.data(),cmdName.data());
    return;
  } 
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  HtmlAttribList attrList;
  DocImage *img = new DocImage(this,attrList,findAndCopyImage(g_token->name,t),t);
  m_children.append(img);
  img->parse();
}

void DocPara::handleDotFile(const QString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  QString name = g_token->name;
  DocDotFile *df = new DocDotFile(this,name);
  m_children.append(df);
  df->parse();
}

void DocPara::handleLink(const QString &cmdName,bool isJavaLink)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  doctokenizerYYsetStateLink();
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  doctokenizerYYsetStatePara();
  DocLink *lnk = new DocLink(this,g_token->name);
  m_children.append(lnk);
  QString leftOver = lnk->parse(isJavaLink);
  if (!leftOver.isEmpty())
  {
    m_children.append(new DocWord(this,leftOver));
  }
}

void DocPara::handleRef(const QString &cmdName)
{
  DBG(("handleRef(%s)\n",cmdName.data()));
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  doctokenizerYYsetStateRef();
  tok=doctokenizerYYlex(); // get the reference id
  DocRef *ref=0;
  if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    goto endref;
  }
  ref = new DocRef(this,g_token->name);
  m_children.append(ref);
  ref->parse();
endref:
  doctokenizerYYsetStatePara();
}


void DocPara::handleInclude(const QString &cmdName,DocInclude::Type t)
{
  DBG(("handleInclude(%s)\n",cmdName.data()));
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
        "argument of command %s",cmdName.data());
    return;
  }
  else if (tok!=TK_WORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  DocInclude *inc = new DocInclude(this,g_token->name,g_context,t,g_isExample,g_exampleName);
  m_children.append(inc);
  inc->parse();
}

void DocPara::handleSection(const QString &cmdName)
{
  // get the argument of the section command.
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
        cmdName.data());
    return;
  }
  tok=doctokenizerYYlex();
  if (tok==0)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
        "argument of command %s\n", cmdName.data());
    return;
  }
  else if (tok!=TK_WORD && tok!=TK_LNKWORD)
  {
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
        tokToString(tok),cmdName.data());
    return;
  }
  g_token->sectionId = g_token->name;
  doctokenizerYYsetStateSkipTitle();
  doctokenizerYYlex();
  doctokenizerYYsetStatePara();
}

int DocPara::handleHtmlHeader(const HtmlAttribList &tagHtmlAttribs,int level)
{
  DocHtmlHeader *header = new DocHtmlHeader(this,tagHtmlAttribs,level);
  m_children.append(header);
  int retval = header->parse();
  return (retval==RetVal_OK) ? TK_NEWPARA : retval;
}

// For XML tags whose content is stored in attributes rather than
// contained within the element, we need a way to inject the attribute
// text into the current paragraph.
bool DocPara::injectToken(int tok,const QString &tokText) 
{
  g_token->name = tokText;
  return defaultHandleToken(this,tok,m_children);
}

int DocPara::handleStartCode()
{
  int retval = doctokenizerYYlex();
  // search for the first non-whitespace line, index is stored in li
  int i=0,li=0,l=g_token->verb.length();
  while (i<l && g_token->verb.at(i)==' ' || g_token->verb.at(i)=='\n')
  {
    if (g_token->verb.at(i)=='\n') li=i+1;
    i++;
  }
  m_children.append(new DocVerbatim(this,g_context,g_token->verb.mid(li),DocVerbatim::Code,g_isExample,g_exampleName));
  if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: code section ended without end marker");
  doctokenizerYYsetStatePara();
  return retval;
}

void DocPara::handleInheritDoc()
{
  if (g_memberDef) // inheriting docs from a member
  {
    MemberDef *reMd = g_memberDef->reimplements();
    if (reMd) // member from which was inherited.
    {
      MemberDef *thisMd = g_memberDef;
      //printf("{InheritDocs:%s=>%s}\n",g_memberDef->qualifiedName().data(),reMd->qualifiedName().data());
      docParserPushContext();
      g_context=reMd->getOuterScope()->name();
      g_memberDef=reMd;
      g_styleStack.clear();
      g_nodeStack.clear();
      g_copyStack.append(reMd);
      internalValidatingParseDoc(this,m_children,reMd->briefDescription());
      internalValidatingParseDoc(this,m_children,reMd->documentation());
      g_copyStack.remove(reMd);
      docParserPopContext();
      g_memberDef = thisMd;
    }
  }
}


int DocPara::handleCommand(const QString &cmdName)
{
  DBG(("handleCommand(%s)\n",cmdName.data()));
  int retval = RetVal_OK;
  switch (Mappers::cmdMapper->map(cmdName))
  {
    case CMD_UNKNOWN:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Found unknown command `\\%s'",cmdName.data());
      break;
    case CMD_EMPHASIS:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,FALSE));
      if (retval!=TK_WORD) m_children.append(new DocWhiteSpace(this," "));
      break;
    case CMD_BOLD:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Bold,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Bold,FALSE));
      if (retval!=TK_WORD) m_children.append(new DocWhiteSpace(this," "));
      break;
    case CMD_CODE:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Code,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Code,FALSE));
      if (retval!=TK_WORD) m_children.append(new DocWhiteSpace(this," "));
      break;
    case CMD_BSLASH:
      m_children.append(new DocSymbol(this,DocSymbol::BSlash));
      break;
    case CMD_AT:
      m_children.append(new DocSymbol(this,DocSymbol::At));
      break;
    case CMD_LESS:
      m_children.append(new DocSymbol(this,DocSymbol::Less));
      break;
    case CMD_GREATER:
      m_children.append(new DocSymbol(this,DocSymbol::Greater));
      break;
    case CMD_AMP:
      m_children.append(new DocSymbol(this,DocSymbol::Amp));
      break;
    case CMD_DOLLAR:
      m_children.append(new DocSymbol(this,DocSymbol::Dollar));
      break;
    case CMD_HASH:
      m_children.append(new DocSymbol(this,DocSymbol::Hash));
      break;
    case CMD_PERCENT:
      m_children.append(new DocSymbol(this,DocSymbol::Percent));
      break;
    case CMD_SA:
      g_inSeeBlock=TRUE;
      retval = handleSimpleSection(DocSimpleSect::See);
      g_inSeeBlock=FALSE;
      break;
    case CMD_RETURN:
      retval = handleSimpleSection(DocSimpleSect::Return);
      g_hasReturnCommand=TRUE;
      break;
    case CMD_AUTHOR:
      retval = handleSimpleSection(DocSimpleSect::Author);
      break;
    case CMD_AUTHORS:
      retval = handleSimpleSection(DocSimpleSect::Authors);
      break;
    case CMD_VERSION:
      retval = handleSimpleSection(DocSimpleSect::Version);
      break;
    case CMD_SINCE:
      retval = handleSimpleSection(DocSimpleSect::Since);
      break;
    case CMD_DATE:
      retval = handleSimpleSection(DocSimpleSect::Date);
      break;
    case CMD_NOTE:
      retval = handleSimpleSection(DocSimpleSect::Note);
      break;
    case CMD_WARNING:
      retval = handleSimpleSection(DocSimpleSect::Warning);
      break;
    case CMD_PRE:
      retval = handleSimpleSection(DocSimpleSect::Pre);
      break;
    case CMD_POST:
      retval = handleSimpleSection(DocSimpleSect::Post);
      break;
    case CMD_INVARIANT:
      retval = handleSimpleSection(DocSimpleSect::Invar);
      break;
    case CMD_REMARK:
      retval = handleSimpleSection(DocSimpleSect::Remark);
      break;
    case CMD_ATTENTION:
      retval = handleSimpleSection(DocSimpleSect::Attention);
      break;
    case CMD_PAR:
      retval = handleSimpleSection(DocSimpleSect::User);
      break;
    case CMD_LI:
      {
	DocSimpleList *sl=new DocSimpleList(this);
	m_children.append(sl);
        retval = sl->parse();
      }
      break;
    case CMD_SECTION:
      {
        handleSection(cmdName);
	retval = RetVal_Section;
      }
      break;
    case CMD_SUBSECTION:
      {
        handleSection(cmdName);
	retval = RetVal_Subsection;
      }
      break;
    case CMD_SUBSUBSECTION:
      {
        handleSection(cmdName);
	retval = RetVal_Subsubsection;
      }
      break;
    case CMD_PARAGRAPH:
      {
        handleSection(cmdName);
	retval = RetVal_Paragraph;
      }
      break;
    case CMD_STARTCODE:
      {
        doctokenizerYYsetStateCode();
        retval = handleStartCode();
      }
      break;
    case CMD_HTMLONLY:
      {
        doctokenizerYYsetStateHtmlOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::HtmlOnly,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: htmlonly section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_MANONLY:
      {
        doctokenizerYYsetStateManOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::ManOnly,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: manonly section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_LATEXONLY:
      {
        doctokenizerYYsetStateLatexOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::LatexOnly,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: latexonly section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_XMLONLY:
      {
        doctokenizerYYsetStateXmlOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::XmlOnly,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: xmlonly section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_VERBATIM:
      {
        doctokenizerYYsetStateVerbatim();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::Verbatim,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: verbatim section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_DOT:
      {
        doctokenizerYYsetStateDot();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_context,g_token->verb,DocVerbatim::Dot,g_isExample,g_exampleName));
        if (retval==0) warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: dot section ended without end marker");
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_ENDCODE:
    case CMD_ENDHTMLONLY:
    case CMD_ENDMANONLY:
    case CMD_ENDLATEXONLY:
    case CMD_ENDXMLONLY:
    case CMD_ENDLINK:
    case CMD_ENDVERBATIM:
    case CMD_ENDDOT:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected command %s",g_token->name.data());
      break; 
    case CMD_PARAM:
      retval = handleParamSection(cmdName,DocParamSect::Param,FALSE,g_token->paramDir);
      break;
    case CMD_RETVAL:
      retval = handleParamSection(cmdName,DocParamSect::RetVal);
      break;
    case CMD_EXCEPTION:
      retval = handleParamSection(cmdName,DocParamSect::Exception);
      break;
    case CMD_XREFITEM:
      retval = handleXRefItem();
      break;
    case CMD_LINEBREAK:
      {
        DocLineBreak *lb = new DocLineBreak(this);
        m_children.append(lb);
      }
      break;
    case CMD_ANCHOR:
      {
	int tok=doctokenizerYYlex();
	if (tok!=TK_WHITESPACE)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
	      cmdName.data());
	  break;
	}
	tok=doctokenizerYYlex();
	if (tok==0)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
	      "argument of command %s",cmdName.data());
	  break;
	}
	else if (tok!=TK_WORD && tok!=TK_LNKWORD)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
	      tokToString(tok),cmdName.data());
	  break;
	}
        DocAnchor *anchor = new DocAnchor(this,g_token->name,FALSE);
        m_children.append(anchor);
      }
      break;
    case CMD_ADDINDEX:
      {
        DocIndexEntry *ie = new DocIndexEntry(this);
        m_children.append(ie);
        retval = ie->parse();
      }
      break;
    case CMD_INTERNAL:
      retval = RetVal_Internal;
      break;
    case CMD_COPYDOC:
      {
	int tok=doctokenizerYYlex();
	if (tok!=TK_WHITESPACE)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: expected whitespace after %s command",
	      cmdName.data());
	  break;
	}
	tok=doctokenizerYYlex();
	if (tok==0)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected end of comment block while parsing the "
	      "argument of command %s\n", cmdName.data());
	  break;
	}
	else if (tok!=TK_WORD && tok!=TK_LNKWORD)
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected token %s as the argument of %s",
	      tokToString(tok),cmdName.data());
	  break;
	}
        DocCopy *cpy = new DocCopy(this,g_token->name);
        m_children.append(cpy);
        cpy->parse();
      }
      break;
    case CMD_INCLUDE:
      handleInclude(cmdName,DocInclude::Include);
      break;
    case CMD_INCWITHLINES:
      handleInclude(cmdName,DocInclude::IncWithLines);
      break;
    case CMD_DONTINCLUDE:
      handleInclude(cmdName,DocInclude::DontInclude);
      break;
    case CMD_HTMLINCLUDE:
      handleInclude(cmdName,DocInclude::HtmlInclude);
      break;
    case CMD_VERBINCLUDE:
      handleInclude(cmdName,DocInclude::VerbInclude);
      break;
    case CMD_SKIP:
      handleIncludeOperator(cmdName,DocIncOperator::Skip);
      break;
    case CMD_UNTIL:
      handleIncludeOperator(cmdName,DocIncOperator::Until);
      break;
    case CMD_SKIPLINE:
      handleIncludeOperator(cmdName,DocIncOperator::SkipLine);
      break;
    case CMD_LINE:
      handleIncludeOperator(cmdName,DocIncOperator::Line);
      break;
    case CMD_IMAGE:
      handleImage(cmdName);
      break;
    case CMD_DOTFILE:
      handleDotFile(cmdName);
      break;
    case CMD_LINK:
      handleLink(cmdName,FALSE);
      break;
    case CMD_JAVALINK:
      handleLink(cmdName,TRUE);
      break;
    case CMD_REF: // fall through
    case CMD_SUBPAGE:
      handleRef(cmdName);
      break;
    case CMD_SECREFLIST:
      {
        DocSecRefList *list = new DocSecRefList(this);
        m_children.append(list);
        list->parse();
      }
      break;
    case CMD_SECREFITEM:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected command %s",g_token->name.data());
      break;
    case CMD_ENDSECREFLIST:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected command %s",g_token->name.data());
      break;
    case CMD_FORMULA:
      {
        DocFormula *form=new DocFormula(this,g_token->id);
        m_children.append(form);
      }
      break;
    //case CMD_LANGSWITCH:
    //  retval = handleLanguageSwitch();
    //  break;
    case CMD_INTERNALREF:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: unexpected command %s",g_token->name.data());
      break;
    case CMD_INHERITDOC:
      handleInheritDoc();
      break;
    default:
      // we should not get here!
      ASSERT(0);
      break;
  }
  INTERNAL_ASSERT(retval==0 || retval==RetVal_OK || retval==RetVal_SimpleSec || 
         retval==TK_LISTITEM || retval==TK_ENDLIST || retval==TK_NEWPARA ||
         retval==RetVal_Section || retval==RetVal_EndList || 
         retval==RetVal_Internal || retval==RetVal_SwitchLang
        );
  DBG(("handleCommand(%s) end retval=%x\n",cmdName.data(),retval));
  return retval;
}

static bool findAttribute(const HtmlAttribList &tagHtmlAttribs, 
                          const char *attrName, 
                          QString *result) 
{

  HtmlAttribListIterator li(tagHtmlAttribs);
  HtmlAttrib *opt;
  for (li.toFirst();(opt=li.current());++li)
  {
    if (opt->name==attrName) 
    {
      *result = opt->value;
      return TRUE;
    }
  }
  return FALSE;
}

int DocPara::handleHtmlStartTag(const QString &tagName,const HtmlAttribList &tagHtmlAttribs)
{
  DBG(("handleHtmlStartTag(%s,%d)\n",tagName.data(),tagHtmlAttribs.count()));
  int retval=RetVal_OK;
  int tagId = Mappers::htmlTagMapper->map(tagName);
  if (g_token->emptyTag && !(tagId&XML_CmdMask) && tagId!=HTML_UNKNOWN)
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: HTML tags may not use the 'empty tag' XHTML syntax.");
  switch (tagId)
  {
    case HTML_UL: 
      {
        DocHtmlList *list = new DocHtmlList(this,tagHtmlAttribs,DocHtmlList::Unordered);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_OL: 
      {
        DocHtmlList *list = new DocHtmlList(this,tagHtmlAttribs,DocHtmlList::Ordered);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_LI:
      if (!insideUL(this) && !insideOL(this))
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: lonely <li> tag found");
      }
      else
      {
        retval=RetVal_ListItem;
      }
      break;
    case HTML_BOLD:
      handleStyleEnter(this,m_children,DocStyleChange::Bold,&g_token->attribs);
      break;
    case HTML_CODE:
      if (g_fileName.right(3)==".cs") 
        // for C# code we treat <code> as an XML tag
      {
        doctokenizerYYsetStateXmlCode();
        retval = handleStartCode();
      }
      else // normal HTML markup
      {
        handleStyleEnter(this,m_children,DocStyleChange::Code,&g_token->attribs);
      }
      break;
    case HTML_EMPHASIS:
      handleStyleEnter(this,m_children,DocStyleChange::Italic,&g_token->attribs);
      break;
    case HTML_DIV:
      handleStyleEnter(this,m_children,DocStyleChange::Div,&g_token->attribs);
      break;
    case HTML_SPAN:
      handleStyleEnter(this,m_children,DocStyleChange::Span,&g_token->attribs);
      break;
    case HTML_SUB:
      handleStyleEnter(this,m_children,DocStyleChange::Subscript,&g_token->attribs);
      break;
    case HTML_SUP:
      handleStyleEnter(this,m_children,DocStyleChange::Superscript,&g_token->attribs);
      break;
    case HTML_CENTER:
      handleStyleEnter(this,m_children,DocStyleChange::Center,&g_token->attribs);
      break;
    case HTML_SMALL:
      handleStyleEnter(this,m_children,DocStyleChange::Small,&g_token->attribs);
      break;
    case HTML_PRE:
      handleStyleEnter(this,m_children,DocStyleChange::Preformatted,&g_token->attribs);
      setInsidePreformatted(TRUE);
      //doctokenizerYYsetInsidePre(TRUE);
      break;
    case HTML_P:
      retval=TK_NEWPARA;
      break;
    case HTML_DL:
      {
        DocHtmlDescList *list = new DocHtmlDescList(this,tagHtmlAttribs);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_DT:
      retval = RetVal_DescTitle;
      break;
    case HTML_DD:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag <dd> found");
      break;
    case HTML_TABLE:
      {
        DocHtmlTable *table = new DocHtmlTable(this,tagHtmlAttribs);
        m_children.append(table);
        retval=table->parse();
      }
      break;
    case HTML_TR:
      retval = RetVal_TableRow;
      break;
    case HTML_TD:
      retval = RetVal_TableCell;
      break;
    case HTML_TH:
      retval = RetVal_TableHCell;
      break;
    case HTML_CAPTION:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag <caption> found");
      break;
    case HTML_BR:
      {
        DocLineBreak *lb = new DocLineBreak(this);
        m_children.append(lb);
      }
      break;
    case HTML_HR:
      {
        DocHorRuler *hr = new DocHorRuler(this);
        m_children.append(hr);
      }
      break;
    case HTML_A:
      retval=handleAHref(this,m_children,tagHtmlAttribs);
      break;
    case HTML_H1:
      retval=handleHtmlHeader(tagHtmlAttribs,1);
      break;
    case HTML_H2:
      retval=handleHtmlHeader(tagHtmlAttribs,2);
      break;
    case HTML_H3:
      retval=handleHtmlHeader(tagHtmlAttribs,3);
      break;
    case HTML_H4:
      retval=handleHtmlHeader(tagHtmlAttribs,4);
      break;
    case HTML_H5:
      retval=handleHtmlHeader(tagHtmlAttribs,5);
      break;
    case HTML_H6:
      retval=handleHtmlHeader(tagHtmlAttribs,6);
      break;
    case HTML_IMG:
      {
        HtmlAttribListIterator li(tagHtmlAttribs);
        HtmlAttrib *opt;
        bool found=FALSE;
        int index=0;
        for (li.toFirst();(opt=li.current());++li,++index)
        {
          //printf("option name=%s value=%s\n",opt->name.data(),opt->value.data());
          if (opt->name=="src" && !opt->value.isEmpty())
          {
            // copy attributes
            HtmlAttribList attrList = tagHtmlAttribs;
            // and remove the href attribute
            bool result = attrList.remove(index);
            ASSERT(result);
            DocImage *img = new DocImage(this,attrList,opt->value,DocImage::Html);
            m_children.append(img);
            found = TRUE;
          }
        }
        if (!found)
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: IMG tag does not have a SRC attribute!\n");
        }
      }
      break;

    case XML_SUMMARY:
    case XML_REMARKS:
    case XML_VALUE:
    case XML_PARA:
      if (!m_children.isEmpty())
      {
        retval = TK_NEWPARA;
      }
      break;
    case XML_EXAMPLE:
    case XML_DESCRIPTION:
      break;
    case XML_C:
      handleStyleEnter(this,m_children,DocStyleChange::Code,&g_token->attribs);
      break;
    case XML_PARAM:
      {
        QString paramName;
        if (findAttribute(tagHtmlAttribs,"name",&paramName))
	{
          retval = handleParamSection(paramName,DocParamSect::Param,TRUE);
        }
        else
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Missing 'name' attribute from <param> tag.");
        }
      }
      break;
    case XML_PARAMREF:
      {
        QString paramName;
        if (findAttribute(tagHtmlAttribs,"name",&paramName))
        {
          m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,TRUE));
          retval=handleStyleArgument(this,m_children,paramName); 
          m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,FALSE));
          if (retval!=TK_WORD) m_children.append(new DocWhiteSpace(this," "));
        }
        else
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Missing 'name' attribute from <paramref> tag.");
        }
      }
      break;
    case XML_EXCEPTION:
      {
        QString exceptName;
        if (findAttribute(tagHtmlAttribs,"cref",&exceptName))
	{
          retval = handleParamSection(exceptName,DocParamSect::Exception,TRUE);
        }
        else
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Missing 'name' attribute from <exception> tag.");
        }
      }
      break;
    case XML_ITEM:
      if (!insideUL(this) && !insideOL(this))
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: lonely <item> tag found");
      }
      else
      {
        retval=RetVal_ListItem;
      }
      break;
    case XML_RETURNS:
      retval = handleSimpleSection(DocSimpleSect::Return,TRUE);
      g_hasReturnCommand=TRUE;
      break;
    case XML_SEE:
      // I'm not sure if <see> is the same as <seealso> or if it
      // should you link a member without producing a section. The
      // C# specification is extremely vague about this (but what else 
      // can we expect from Microsoft...)
      {
        QString cref;
        if (findAttribute(tagHtmlAttribs,"cref",&cref))
        {
          DocRef *ref = new DocRef(this,cref);
          m_children.append(ref);
        }
        else
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Missing 'cref' attribute from <see> tag.");
        }
      }
      break;
    case XML_SEEALSO:
      {
        QString cref;
        if (findAttribute(tagHtmlAttribs,"cref",&cref))
	{
	  // Look for an existing "see" section
          DocSimpleSect *ss=0;
          QListIterator<DocNode> cli(m_children);
          DocNode *n;
          for (cli.toFirst();(n=cli.current());++cli)
          {
            if (n->kind()==Kind_SimpleSect && ((DocSimpleSect *)n)->type()==DocSimpleSect::See)
            {
              ss = (DocSimpleSect *)n;
            }
          }
  
          if (!ss)  // start new section
          {
            ss=new DocSimpleSect(this,DocSimpleSect::See);
            m_children.append(ss);
          }
          
          ss->appendLinkWord(cref);
	  retval = RetVal_OK;
        }
        else
        {
          warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Missing 'cref' attribute from <seealso> tag.");
        }
      }
      break;
    case XML_LIST:
      {
        QString type;
        DocHtmlList::Type listType = DocHtmlList::Unordered;
        if (findAttribute(tagHtmlAttribs,"type",&type) && type=="number")
        {
          listType=DocHtmlList::Ordered;
        }
        DocHtmlList *list = new DocHtmlList(this,tagHtmlAttribs,listType);
        m_children.append(list);
        retval=list->parseXml();
      }
      break;
    case XML_INCLUDE:
    case XML_PERMISSION:
      // These tags are defined in .Net but are currently unsupported
      break;
    case HTML_UNKNOWN:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported xml/html tag <%s> found", tagName.data());
      m_children.append(new DocWord(this, "<"+tagName+tagHtmlAttribs.toString()+">"));
      break;
    default:
      // we should not get here!
      ASSERT(0);
      break;
  }
  return retval;
}

int DocPara::handleHtmlEndTag(const QString &tagName)
{
  DBG(("handleHtmlEndTag(%s)\n",tagName.data()));
  int tagId = Mappers::htmlTagMapper->map(tagName);
  int retval=RetVal_OK;
  switch (tagId)
  {
    case HTML_UL: 
      if (!insideUL(this))
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </ul> tag without matching <ul>");
      }
      else
      {
        retval=RetVal_EndList;
      }
      break;
    case HTML_OL: 
      if (!insideOL(this))
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </ol> tag without matching <ol>");
      }
      else
      {
        retval=RetVal_EndList;
      }
      break;
    case HTML_LI:
      if (!insideLI(this))
      {
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </li> tag without matching <li>");
      }
      else
      {
        // ignore </li> tags
      }
      break;
    //case HTML_PRE:
    //  if (!insidePRE(this))
    //  {
    //    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found </pre> tag without matching <pre>");
    //  }
    //  else
    //  {
    //    retval=RetVal_EndPre;
    //  }
    //  break;
    case HTML_BOLD:
      handleStyleLeave(this,m_children,DocStyleChange::Bold,"b");
      break;
    case HTML_CODE:
      handleStyleLeave(this,m_children,DocStyleChange::Code,"code");
      break;
    case HTML_EMPHASIS:
      handleStyleLeave(this,m_children,DocStyleChange::Italic,"em");
      break;
    case HTML_DIV:
      handleStyleLeave(this,m_children,DocStyleChange::Div,"div");
      break;
    case HTML_SPAN:
      handleStyleLeave(this,m_children,DocStyleChange::Span,"span");
      break;
    case HTML_SUB:
      handleStyleLeave(this,m_children,DocStyleChange::Subscript,"sub");
      break;
    case HTML_SUP:
      handleStyleLeave(this,m_children,DocStyleChange::Superscript,"sup");
      break;
    case HTML_CENTER:
      handleStyleLeave(this,m_children,DocStyleChange::Center,"center");
      break;
    case HTML_SMALL:
      handleStyleLeave(this,m_children,DocStyleChange::Small,"small");
      break;
    case HTML_PRE:
      handleStyleLeave(this,m_children,DocStyleChange::Preformatted,"pre");
      setInsidePreformatted(FALSE);
      //doctokenizerYYsetInsidePre(FALSE);
      break;
    case HTML_P:
      // ignore </p> tag
      break;
    case HTML_DL:
      retval=RetVal_EndDesc;
      break;
    case HTML_DT:
      // ignore </dt> tag
      break;
    case HTML_DD:
      // ignore </dd> tag
      break;
    case HTML_TABLE:
      retval=RetVal_EndTable;
      break;
    case HTML_TR:
      // ignore </tr> tag
      break;
    case HTML_TD:
      // ignore </td> tag
      break;
    case HTML_TH:
      // ignore </th> tag
      break;
    case HTML_CAPTION:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </caption> found");
      break;
    case HTML_BR:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Illegal </br> tag found\n");
      break;
    case HTML_H1:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </h1> found");
      break;
    case HTML_H2:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </h2> found");
      break;
    case HTML_H3:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </h3> found");
      break;
    case HTML_IMG:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </img> found");
      break;
    case HTML_HR:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </hr> found");
      break;
    case HTML_A:
      //warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected tag </a> found");
      // ignore </a> tag (can be part of <a name=...></a>
      break;

    case XML_SUMMARY:
    case XML_REMARKS:
    case XML_PARA:
    case XML_VALUE:
    case XML_LIST:
    case XML_EXAMPLE:
    case XML_PARAM:
    case XML_RETURNS:
    case XML_SEEALSO:
    case XML_EXCEPTION:
      retval = RetVal_CloseXml;
      break;
    case XML_C:
      handleStyleLeave(this,m_children,DocStyleChange::Code,"c");
      break;
    case XML_ITEM:
    case XML_INCLUDE:
    case XML_PERMISSION:
    case XML_DESCRIPTION:
    case XML_PARAMREF:
      // These tags are defined in .Net but are currently unsupported
      break;
    case HTML_UNKNOWN:
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported xml/html tag </%s> found", tagName.data());
      m_children.append(new DocWord(this,"</"+tagName+">"));
      break;
    default:
      // we should not get here!
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Unexpected end tag %s\n",tagName.data());
      ASSERT(0);
      break;
  }
  return retval;
}

int DocPara::parse()
{
  DBG(("DocPara::parse() start\n"));
  g_nodeStack.push(this);
  // handle style commands "inherited" from the previous paragraph
  handleInitialStyleCommands(this,m_children);
  int tok;
  int retval=0;
  while ((tok=doctokenizerYYlex())) // get the next token
  {
reparsetoken:
    DBG(("token %s at %d",tokToString(tok),doctokenizerYYlineno));
    if (tok==TK_WORD || tok==TK_LNKWORD || tok==TK_SYMBOL || tok==TK_URL || 
        tok==TK_COMMAND || tok==TK_HTMLTAG
       )
    {
      DBG((" name=%s",g_token->name.data()));
    }
    DBG(("\n"));
    switch(tok)
    {
      case TK_WORD:        
	m_children.append(new DocWord(this,g_token->name));
	break;
      case TK_LNKWORD:        
        handleLinkedWord(this,m_children);
	break;
      case TK_URL:
        m_children.append(new DocURL(this,g_token->name,g_token->isEMailAddr));
        break;
      case TK_WHITESPACE:  
        {
          // prevent leading whitespace and collapse multiple whitespace areas
          DocNode::Kind k; 
          if (insidePRE(this) || // all whitespace is relevant
              (                
               // remove leading whitespace 
               !m_children.isEmpty()  && 
               // and whitespace after certain constructs
               (k=m_children.last()->kind())!=DocNode::Kind_HtmlDescList &&
               k!=DocNode::Kind_HtmlTable &&
               k!=DocNode::Kind_HtmlList &&
               k!=DocNode::Kind_SimpleSect &&
               k!=DocNode::Kind_AutoList &&
               k!=DocNode::Kind_SimpleList &&
               /*k!=DocNode::Kind_Verbatim &&*/
               k!=DocNode::Kind_HtmlHeader &&
               k!=DocNode::Kind_ParamSect &&
               k!=DocNode::Kind_XRefItem
              )
             )
          {
            m_children.append(new DocWhiteSpace(this,g_token->chars));
          }
        }
	break;
      case TK_LISTITEM:    
	{
	  DBG(("found list item at %d parent=%d\n",g_token->indent,parent()->kind()));
	  DocNode *n=parent();
	  while (n && n->kind()!=DocNode::Kind_AutoList) n=n->parent();
	  if (n) // we found an auto list up in the hierarchy
	  {
	    DocAutoList *al = (DocAutoList *)n;
	    DBG(("previous list item at %d\n",al->indent()));
	    if (al->indent()>=g_token->indent) 
	      // new item at the same or lower indent level
	    {
	      retval=TK_LISTITEM;
	      goto endparagraph;
	    }
	  }

          // determine list depth
          int depth = 0;
          n=parent();
          while(n) {
            if(n->kind() == DocNode::Kind_AutoList) ++depth;
            n=n->parent();
          }

	  // first item or sub list => create new list
	  DocAutoList *al=0;
	  do
	  {
	    al = new DocAutoList(this,g_token->indent,g_token->isEnumList,
                                 depth);
	    m_children.append(al);
	    retval = al->parse();
	  } while (retval==TK_LISTITEM &&         // new list
	           al->indent()==g_token->indent  // at same indent level
		  );
	      
	  // check the return value
	  if (retval==RetVal_SimpleSec) // auto list ended due to simple section command
	  {
	    // Reparse the token that ended the section at this level,
	    // so a new simple section will be started at this level.
	    // This is the same as unputting the last read token and continuing.
	    g_token->name = g_token->simpleSectName;
	    tok = TK_COMMAND;
	    DBG(("reparsing command %s\n",g_token->name.data()));
	    goto reparsetoken;
	  }
	  else if (retval==TK_ENDLIST)
	  {
	    if (al->indent()>g_token->indent) // end list
	    {
	      goto endparagraph;
	    }
	    else // continue with current paragraph
	    {
	    }
	  }
	  else // paragraph ended due to TK_NEWPARA, TK_LISTITEM, or EOF
	  {
	    goto endparagraph;
	  }
	}
	break;
      case TK_ENDLIST:     
	DBG(("Found end of list inside of paragraph at line %d\n",doctokenizerYYlineno));
	if (parent()->kind()==DocNode::Kind_AutoListItem)
	{
	  ASSERT(parent()->parent()->kind()==DocNode::Kind_AutoList);
	  DocAutoList *al = (DocAutoList *)parent()->parent();
	  if (al->indent()>=g_token->indent)
	  {
	    // end of list marker ends this paragraph
	    retval=TK_ENDLIST;
	    goto endparagraph;
	  }
	  else
	  {
	    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: End of list marker found "
		   "has invalid indent level");
	  }
	}
	else
	{
	  warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: End of list marker found without any preceding "
	         "list items");
	}
	break;
      case TK_COMMAND:    
	{
	  // see if we have to start a simple section
	  int cmd = Mappers::cmdMapper->map(g_token->name);
	  DocNode *n=parent();
	  while (n && 
                 n->kind()!=DocNode::Kind_SimpleSect && 
                 n->kind()!=DocNode::Kind_ParamSect
                ) 
          {
            n=n->parent();
          }
	  if (cmd&SIMPLESECT_BIT)
	  {
	    if (n)  // already in a simple section
	    {
	      // simple section cannot start in this paragraph, need
	      // to unwind the stack and remember the command.
	      g_token->simpleSectName = g_token->name.copy();
	      retval=RetVal_SimpleSec;
	      goto endparagraph;
	    }
	  }
	  // see if we are in a simple list
	  n=parent();
	  while (n && n->kind()!=DocNode::Kind_SimpleListItem) n=n->parent();
	  if (n)
	  {
	    if (cmd==CMD_LI)
	    {
	      retval=RetVal_ListItem;
	      goto endparagraph;
	    }
	  }
	  
	  // handle the command
	  retval=handleCommand(g_token->name.copy());
          DBG(("handleCommand returns %x\n",retval));

	  // check the return value
	  if (retval==RetVal_SimpleSec)
	  {
	    // Reparse the token that ended the section at this level,
	    // so a new simple section will be started at this level.
	    // This is the same as unputting the last read token and continuing.
	    g_token->name = g_token->simpleSectName;
	    tok = TK_COMMAND;
	    DBG(("reparsing command %s\n",g_token->name.data()));
	    goto reparsetoken;
	  }
	  else if (retval==RetVal_OK) 
	  {
	    // the command ended normally, keep scanning for new tokens.
	    retval = 0;
	  }
          else if (retval>0 && retval<RetVal_OK)
          { 
            // the command ended with a new command, reparse this token
            tok = retval;
            goto reparsetoken;
          }
	  else // end of file, end of paragraph, start or end of section 
	       // or some auto list marker
	  {
	    goto endparagraph;
	  }
	}
	break;
      case TK_HTMLTAG:    
        {
          if (!g_token->endTag) // found a start tag
          {
            retval = handleHtmlStartTag(g_token->name,g_token->attribs);
          }
          else // found an end tag
          {
            retval = handleHtmlEndTag(g_token->name);
          }
          if (retval==RetVal_OK) 
          {
	    // the command ended normally, keep scanner for new tokens.
            retval = 0;
          }
          else
          {
            goto endparagraph;
          }
        }
	break;
      case TK_SYMBOL:     
        {
          char letter='\0';
          DocSymbol::SymType s = DocSymbol::decodeSymbol(g_token->name,&letter);
          if (s!=DocSymbol::Unknown)
          {
            m_children.append(new DocSymbol(this,s,letter));
          }
          else
          {
            warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
                g_token->name.data());
          }
          break;
        }
      case TK_NEWPARA:     
	retval=TK_NEWPARA;
	goto endparagraph;
      case TK_RCSTAG:
        {
          DocSimpleSect *ss=new DocSimpleSect(this,DocSimpleSect::Rcs);
          m_children.append(ss);
          ss->parseRcs();
        }
        break;
      default:
        warn_doc_error(g_fileName,doctokenizerYYlineno,
            "Warning: Found unexpected token (id=%x)\n",tok);
        break;
    }
  }
  retval=0;
endparagraph:
  handlePendingStyleCommands(this,m_children);
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocPara::parse() end retval=%x\n",retval));
  INTERNAL_ASSERT(retval==0 || retval==TK_NEWPARA || retval==TK_LISTITEM || 
         retval==TK_ENDLIST || retval>RetVal_OK 
	);

  return retval; 
}

//--------------------------------------------------------------------------

int DocSection::parse()
{
  DBG(("DocSection::parse() start %s level=%d\n",g_token->sectionId.data(),m_level));
  int retval=RetVal_OK;
  g_nodeStack.push(this);

  SectionInfo *sec;
  if (!m_id.isEmpty())
  {
    sec=Doxygen::sectionDict[m_id];
    if (sec)
    {
      m_file   = sec->fileName;
      m_anchor = sec->label;
      m_title  = sec->title;
      if (m_title.isEmpty()) m_title = sec->label;
      if (g_sectionDict && g_sectionDict->find(m_id)==0)
      {
        g_sectionDict->insert(m_id,sec);
      }
    }
  }

  // first parse any number of paragraphs
  bool isFirst=TRUE;
  DocPara *lastPar=0;
  do
  {
    DocPara *par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    retval=par->parse();
    if (!par->isEmpty()) 
    {
      m_children.append(par);
      lastPar=par;
    }
    else
    {
      delete par;
    }
    if (retval==TK_LISTITEM)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Invalid list item found");
    }
  } while (retval!=0 && 
           retval!=RetVal_Internal      &&
           retval!=RetVal_Section       &&
           retval!=RetVal_Subsection    &&
           retval!=RetVal_Subsubsection &&
           retval!=RetVal_Paragraph    
          );

  if (lastPar) lastPar->markLast();

  if (retval==RetVal_Subsection && m_level==1)
  {
    // then parse any number of nested sections
    while (retval==RetVal_Subsection) // more sections follow
    {
      //SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
      DocSection *s=new DocSection(this,2,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }
  else if (retval==RetVal_Subsubsection && m_level==2)
  {
    // then parse any number of nested sections
    while (retval==RetVal_Subsubsection) // more sections follow
    {
      //SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
      DocSection *s=new DocSection(this,3,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }
  else if (retval==RetVal_Paragraph && m_level==3)
  {
    // then parse any number of nested sections
    while (retval==RetVal_Paragraph) // more sections follow
    {
      //SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
      DocSection *s=new DocSection(this,4,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }
  else if ((m_level<=1 && retval==RetVal_Subsubsection) ||
           (m_level<=2 && retval==RetVal_Paragraph)
          ) 
  {
    int level; 
    if (retval==RetVal_Subsection) level=2; 
    else if (retval==RetVal_Subsubsection) level=3;
    else level=4;
    warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected %s "
            "command found inside %s!",
            sectionLevelToName[level],sectionLevelToName[m_level]);
    retval=0; // stop parsing
            
  }
  else if (retval==RetVal_Internal)
  {
    DocInternal *in = new DocInternal(this);
    m_children.append(in);
    retval = in->parse(m_level+1);
  }
  else
  {
  }

  INTERNAL_ASSERT(retval==0 || 
                  retval==RetVal_Section || 
                  retval==RetVal_Subsection || 
                  retval==RetVal_Subsubsection || 
                  retval==RetVal_Paragraph || 
                  retval==RetVal_Internal
                 );

  DBG(("DocSection::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

void DocText::parse()
{
  DBG(("DocText::parse() start\n"));
  g_nodeStack.push(this);
  doctokenizerYYsetStateText();
  
  int tok;
  while ((tok=doctokenizerYYlex())) // get the next token
  {
    switch(tok)
    {
      case TK_WORD:        
	m_children.append(new DocWord(this,g_token->name));
	break;
      case TK_WHITESPACE:  
        m_children.append(new DocWhiteSpace(this,g_token->chars));
	break;
      case TK_SYMBOL:     
        {
          char letter='\0';
          DocSymbol::SymType s = DocSymbol::decodeSymbol(g_token->name,&letter);
          if (s!=DocSymbol::Unknown)
          {
            m_children.append(new DocSymbol(this,s,letter));
          }
          else
          {
            warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unsupported symbol %s found",
                g_token->name.data());
          }
        }
        break;
      case TK_COMMAND: 
        switch (Mappers::cmdMapper->map(g_token->name))
        {
          case CMD_BSLASH:
            m_children.append(new DocSymbol(this,DocSymbol::BSlash));
            break;
          case CMD_AT:
            m_children.append(new DocSymbol(this,DocSymbol::At));
            break;
          case CMD_LESS:
            m_children.append(new DocSymbol(this,DocSymbol::Less));
            break;
          case CMD_GREATER:
            m_children.append(new DocSymbol(this,DocSymbol::Greater));
            break;
          case CMD_AMP:
            m_children.append(new DocSymbol(this,DocSymbol::Amp));
            break;
          case CMD_DOLLAR:
            m_children.append(new DocSymbol(this,DocSymbol::Dollar));
            break;
          case CMD_HASH:
            m_children.append(new DocSymbol(this,DocSymbol::Hash));
            break;
          case CMD_PERCENT:
            m_children.append(new DocSymbol(this,DocSymbol::Percent));
            break;
          default:
            warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected command `%s' found",
                      g_token->name.data());
            break;
        }
        break;
      default:
        warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Unexpected token %s",
            tokToString(tok));
        break;
    }
  }

  handleUnclosedStyleCommands();

  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocText::parse() end\n"));
}


//--------------------------------------------------------------------------

void DocRoot::parse()
{
  DBG(("DocRoot::parse() start\n"));
  g_nodeStack.push(this);
  doctokenizerYYsetStatePara();
  int retval=0;

  // first parse any number of paragraphs
  bool isFirst=TRUE;
  DocPara *lastPar=0;
  do
  {
    DocPara *par = new DocPara(this);
    if (isFirst) { par->markFirst(); isFirst=FALSE; }
    retval=par->parse();
    if (!par->isEmpty()) 
    {
      m_children.append(par);
      lastPar=par;
    }
    else
    {
      delete par;
    }
    if (retval==TK_LISTITEM)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Invalid list item found");
    }
    else if (retval==RetVal_Subsection)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found subsection command outside of section context!");
    }
    else if (retval==RetVal_Subsubsection)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found subsubsection command outside of subsection context!");
    }
    else if (retval==RetVal_Paragraph)
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: found paragraph command outside of subsubsection context!");
    }
  } while (retval!=0 && retval!=RetVal_Section && retval!=RetVal_Internal);
  if (lastPar) lastPar->markLast();

  // then parse any number of level1 sections
  while (retval==RetVal_Section)
  {
    SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
    if (sec)
    {
      DocSection *s=new DocSection(this,1,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
    else
    {
      warn_doc_error(g_fileName,doctokenizerYYlineno,"Warning: Invalid section id `%s'; ignoring section",g_token->sectionId.data());
      retval = 0;
    }
  }

  if (retval==RetVal_Internal)
  {
    DocInternal *in = new DocInternal(this);
    m_children.append(in);
    retval = in->parse(1);
  }


  handleUnclosedStyleCommands();

  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocRoot::parse() end\n"));
}

//--------------------------------------------------------------------------

DocNode *validatingParseDoc(const char *fileName,int startLine,
                            Definition *ctx,MemberDef *md,
                            const char *input,bool indexWords,
                            bool isExample, const char *exampleName,
                            bool singleLine, bool linkFromIndex)
{
  
  //printf("validatingParseDoc(%s,%s)\n",ctx?ctx->name().data():"<none>",
  //                                     md?md->name().data():"<none>");
  //printf("========== validating %s at line %d\n",fileName,startLine);
  //printf("---------------- input --------------------\n%s\n----------- end input -------------------\n",input);
  g_token = new TokenInfo;

  if (ctx &&
      (ctx->definitionType()==Definition::TypeClass || 
       ctx->definitionType()==Definition::TypeNamespace
      )
     ) 
  {
    g_context = ctx->name();
  }
  else if (ctx && ctx->definitionType()==Definition::TypePage)
  {
    Definition *scope = ((PageDef*)ctx)->getPageScope();
    if (scope) g_context = scope->name();
  }
  else if (ctx && ctx->definitionType()==Definition::TypeGroup)
  {
    Definition *scope = ((GroupDef*)ctx)->getGroupScope();
    if (scope) g_context = scope->name();
  }
  else
  {
    g_context = "";
  }

  if (indexWords && md && Config_getBool("SEARCHENGINE"))
  {
    g_searchUrl=md->getOutputFileBase();
    Doxygen::searchIndex->setCurrentDoc(
        theTranslator->trMember(TRUE,TRUE)+" "+md->qualifiedName(),
        g_searchUrl,
        md->anchor());
  }
  else if (indexWords && ctx && Config_getBool("SEARCHENGINE"))
  {
    g_searchUrl=ctx->getOutputFileBase();
    QCString name = ctx->qualifiedName();
    if (Config_getBool("OPTIMIZE_OUTPUT_JAVA"))
    {
      name = substitute(name,"::",".");
    }
    switch (ctx->definitionType())
    {
      case Definition::TypePage:
        {
          PageDef *pd = (PageDef *)ctx;
          if (!pd->title().isEmpty())
          {
            name = theTranslator->trPage(TRUE,TRUE)+" "+pd->title();
          }
          else
          {
            name = theTranslator->trPage(TRUE,TRUE)+" "+pd->name();
          }
        }
        break;
      case Definition::TypeClass:
        {
          ClassDef *cd = (ClassDef *)ctx;
          name.prepend(cd->compoundTypeString()+" ");
        }
        break;
      case Definition::TypeNamespace:
        {
          if (Config_getBool("OPTIMIZE_OUTPUT_JAVA"))
          {
            name = theTranslator->trPackage(name);
          }
          else
          {
            name.prepend(theTranslator->trNamespace(TRUE,TRUE)+" ");
          }
        }
        break;
      case Definition::TypeGroup:
        {
          GroupDef *gd = (GroupDef *)ctx;
          if (gd->groupTitle())
          {
            name = theTranslator->trGroup(TRUE,TRUE)+" "+gd->groupTitle();
          }
          else
          {
            name.prepend(theTranslator->trGroup(TRUE,TRUE)+" ");
          }
        }
        break;
      default:
        break;
    }
    Doxygen::searchIndex->setCurrentDoc(name,g_searchUrl);
  }
  else
  {
    g_searchUrl="";
  }

  g_fileName = fileName;
  g_relPath = (!linkFromIndex && ctx) ? 
               QString(relativePathToRoot(ctx->getOutputFileBase())) : 
               QString("");
  //printf("ctx->name=%s relPath=%s\n",ctx->name().data(),g_relPath.data());
  g_memberDef = md;
  g_nodeStack.clear();
  g_styleStack.clear();
  g_initialStyleStack.clear();
  g_inSeeBlock = FALSE;
  g_insideHtmlLink = FALSE;
  g_includeFileText = "";
  g_includeFileOffset = 0;
  g_includeFileLength = 0;
  g_isExample = isExample;
  g_exampleName = exampleName;
  g_hasParamCommand = FALSE;
  g_hasReturnCommand = FALSE;
  g_paramsFound.setAutoDelete(FALSE);
  g_paramsFound.clear();
  g_sectionDict = 0; //sections;
  
  //printf("Starting comment block at %s:%d\n",g_fileName.data(),startLine);
  doctokenizerYYlineno=startLine;
  doctokenizerYYinit(input,g_fileName);

  // build abstract syntax tree
  DocRoot *root = new DocRoot(md!=0,singleLine);
  root->parse();

  if (Debug::isFlagSet(Debug::PrintTree))
  {
    // pretty print the result
    PrintDocVisitor *v = new PrintDocVisitor;
    root->accept(v);
    delete v;
  }

  checkUndocumentedParams();
  detectNoDocumentedParams();

  delete g_token;

  // TODO: These should be called at the end of the program.
  //doctokenizerYYcleanup();
  //Mappers::cmdMapper->freeInstance();
  //Mappers::htmlTagMapper->freeInstance();

  return root;
}

DocNode *validatingParseText(const char *input)
{
  //printf("------------ input ---------\n%s\n"
  //       "------------ end input -----\n",input);
  g_token = new TokenInfo;
  g_context = "";
  g_fileName = "<parseText>";
  g_relPath = "";
  g_memberDef = 0;
  g_nodeStack.clear();
  g_styleStack.clear();
  g_initialStyleStack.clear();
  g_inSeeBlock = FALSE;
  g_insideHtmlLink = FALSE;
  g_includeFileText = "";
  g_includeFileOffset = 0;
  g_includeFileLength = 0;
  g_isExample = FALSE;
  g_exampleName = "";
  g_hasParamCommand = FALSE;
  g_hasReturnCommand = FALSE;
  g_paramsFound.setAutoDelete(FALSE);
  g_paramsFound.clear();
  g_searchUrl="";

  DocText *txt = new DocText;

  if (input)
  {
    doctokenizerYYlineno=1;
    doctokenizerYYinit(input,g_fileName);

    // build abstract syntax tree
    txt->parse();

    if (Debug::isFlagSet(Debug::PrintTree))
    {
      // pretty print the result
      PrintDocVisitor *v = new PrintDocVisitor;
      txt->accept(v);
      delete v;
    }
  }

  delete g_token;

  return txt;
}

void docFindSections(const char *input,
                     Definition *d,
                     MemberGroup *mg,
                     const char *fileName)
{
  doctokenizerYYFindSections(input,d,mg,fileName);
}

void initDocParser()
{
  if (Config_getBool("SEARCHENGINE"))
  {
    Doxygen::searchIndex = new SearchIndex;
  }
  else
  {
    Doxygen::searchIndex = 0;
  }
}

void finializeDocParser()
{
  delete Doxygen::searchIndex;
}

