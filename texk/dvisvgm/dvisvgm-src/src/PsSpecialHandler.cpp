/*************************************************************************
** PsSpecialHandler.cpp                                                 **
**                                                                      **
** This file is part of dvisvgm -- the DVI to SVG converter             **
** Copyright (C) 2005-2015 Martin Gieseking <martin.gieseking@uos.de>   **
**                                                                      **
** This program is free software; you can redistribute it and/or        **
** modify it under the terms of the GNU General Public License as       **
** published by the Free Software Foundation; either version 3 of       **
** the License, or (at your option) any later version.                  **
**                                                                      **
** This program is distributed in the hope that it will be useful, but  **
** WITHOUT ANY WARRANTY; without even the implied warranty of           **
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the         **
** GNU General Public License for more details.                         **
**                                                                      **
** You should have received a copy of the GNU General Public License    **
** along with this program; if not, see <http://www.gnu.org/licenses/>. **
*************************************************************************/

#include <config.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include "EPSFile.h"
#include "FileFinder.h"
#include "Ghostscript.h"
#include "Message.h"
#include "PathClipper.h"
#include "PSPattern.h"
#include "PSPreviewFilter.h"
#include "PsSpecialHandler.h"
#include "SpecialActions.h"
#include "SVGTree.h"
#include "TensorProductPatch.h"
#include "VectorIterator.h"
#include "XMLNode.h"
#include "XMLString.h"
#include "TriangularPatch.h"


using namespace std;


static inline double str2double (const string &str) {
	double ret;
	istringstream iss(str);
	iss >> ret;
	return ret;
}


bool PsSpecialHandler::COMPUTE_CLIPPATHS_INTERSECTIONS = false;
bool PsSpecialHandler::SHADING_SEGMENT_OVERLAP = false;
int PsSpecialHandler::SHADING_SEGMENT_SIZE = 20;
double PsSpecialHandler::SHADING_SIMPLIFY_DELTA = 0.01;


PsSpecialHandler::PsSpecialHandler () : _psi(this), _actions(0), _previewFilter(_psi), _psSection(PS_NONE), _xmlnode(0)
{
}


PsSpecialHandler::~PsSpecialHandler () {
	_psi.setActions(0);     // ensure no further PS actions are performed
	for (map<int, PSPattern*>::iterator it=_patterns.begin(); it != _patterns.end(); ++it)
		delete it->second;
}


/** Initializes the PostScript handler. It's called by the first use of process(). The
 *  deferred initialization speeds up the conversion of DVI files that doesn't contain
 *  PS specials. */
void PsSpecialHandler::initialize () {
	if (_psSection == PS_NONE) {
		// initial values of graphics state
		_linewidth = 1;
		_linecap = _linejoin = 0;
		_miterlimit = 4;
		_xmlnode = _savenode = 0;
		_opacityalpha = 1;  // fully opaque
		_sx = _sy = _cos = 1.0;
		_pattern = 0;

		// execute dvips prologue/header files
		const char *headers[] = {"tex.pro", "texps.pro", "special.pro", /*"color.pro",*/ 0};
		for (const char **p=headers; *p; ++p)
			processHeaderFile(*p);
		// disable bop/eop operators to prevent side-effects by
		// unexpected bobs/eops present in PS specials
		_psi.execute("\nTeXDict begin /bop{pop pop}def /eop{}def end ");
		_psSection = PS_HEADERS;  // allow to process header specials now
	}
}


void PsSpecialHandler::processHeaderFile (const char *name) {
	if (const char *path = FileFinder::lookup(name, false)) {
		ifstream ifs(path);
		_psi.execute(string("%%BeginProcSet: ")+name+" 0 0\n", false);
		_psi.execute(ifs, false);
		_psi.execute("%%EndProcSet\n", false);
	}
	else
		Message::wstream(true) << "PostScript header file " << name << " not found\n";
}


void PsSpecialHandler::enterBodySection () {
	if (_psSection == PS_HEADERS) {
		_psSection = PS_BODY; // don't process any PS header code
		ostringstream oss;
		// process collected header code
		if (!_headerCode.empty()) {
			oss << "\nTeXDict begin @defspecial " << _headerCode << "\n@fedspecial end";
			_headerCode.clear();
		}
		// push dictionary "TeXDict" with dvips definitions on dictionary stack
		// and initialize basic dvips PostScript variables
		oss << "\nTeXDict begin 0 0 1000 72 72 () @start 0 0 moveto ";
		_psi.execute(oss.str(), false);
		// Check for information generated by preview.sty. If the tightpage options
		// was set, don't execute the bop-hook but allow the PS filter to read
		// the bbox data present at the beginning of the page.
		_psi.setFilter(&_previewFilter);
		_previewFilter.activate();
		if (!_previewFilter.tightpage())
			_psi.execute("userdict/bop-hook known{bop-hook}if\n", false);
	}
}


/** Move PS graphic position to current DVI location. */
void PsSpecialHandler::moveToDVIPos () {
	if (_actions) {
		const double x = _actions->getX();
		const double y = _actions->getY();
		ostringstream oss;
		oss << '\n' << x << ' ' << y << " moveto ";
		_psi.execute(oss.str());
		_currentpoint = DPair(x, y);
	}
}


/** Executes a PS snippet and optionally synchronizes the DVI cursor position
 *  with the current PS point.
 *  @param[in] is  stream to read the PS code from
 *  @param[in] updatePos if true, move the DVI drawing position to the current PS point */
void PsSpecialHandler::executeAndSync (istream &is, bool updatePos) {
	if (_actions && _actions->getColor() != _currentcolor) {
		// update the PS graphics state if the color has been changed by a color special
		double r, g, b;
		_actions->getColor().getRGB(r, g, b);
		ostringstream oss;
		oss << '\n' << r << ' ' << g << ' ' << b << " setrgbcolor ";
		_psi.execute(oss.str(), false);
	}
	_psi.execute(is);
	if (updatePos) {
		// retrieve current PS position (stored in _currentpoint)
		_psi.execute("\nquerypos ");
		if (_actions) {
			_actions->setX(_currentpoint.x());
			_actions->setY(_currentpoint.y());
		}
	}
}


void PsSpecialHandler::preprocess (const char *prefix, istream &is, SpecialActions *actions) {
	initialize();
	if (_psSection != PS_HEADERS)
		return;

	_actions = actions;
	if (*prefix == '!') {
		_headerCode += "\n";
		_headerCode += string(istreambuf_iterator<char>(is), istreambuf_iterator<char>());
	}
	else if (strcmp(prefix, "header=") == 0) {
		// read and execute PS header file
		string fname;
		is >> fname;
		processHeaderFile(fname.c_str());
	}
}


bool PsSpecialHandler::process (const char *prefix, istream &is, SpecialActions *actions) {
	// process PS headers only once (in prescan)
	if (*prefix == '!' || strcmp(prefix, "header=") == 0)
		return true;

	_actions = actions;
	initialize();
	if (_psSection != PS_BODY)
		enterBodySection();

	if (*prefix == '"') {
		// read and execute literal PostScript code (isolated by a wrapping save/restore pair)
		moveToDVIPos();
		_psi.execute("\n@beginspecial @setspecial ");
		executeAndSync(is, false);
		_psi.execute("\n@endspecial ");
	}
	else if (strcmp(prefix, "psfile=") == 0 || strcmp(prefix, "PSfile=") == 0) {
		if (_actions) {
			StreamInputReader in(is);
			string fname = in.getQuotedString(in.peek() == '"' ? '"' : 0);
			map<string,string> attr;
			in.parseAttributes(attr);
			psfile(fname, attr);
		}
	}
	else if (strcmp(prefix, "ps::") == 0) {
		if (_actions)
			_actions->finishLine();  // reset DVI position on next DVI command
		if (is.peek() == '[') {
			// collect characters inside the brackets
			string code;
			for (int i=0; i < 9 && is.peek() != ']' && !is.eof(); ++i)
				code += is.get();
			if (is.peek() == ']')
				code += is.get();

			if (code == "[begin]" || code == "[nobreak]") {
				moveToDVIPos();
				executeAndSync(is, true);
			}
			else {
				// no move to DVI position here
				if (code != "[end]") // PS array?
					_psi.execute(code);
				executeAndSync(is, true);
			}
		}
		else { // ps::<code> behaves like ps::[end]<code>
			// no move to DVI position here
			executeAndSync(is, true);
		}
	}
	else { // ps: ...
		if (_actions)
			_actions->finishLine();
		moveToDVIPos();
		StreamInputReader in(is);
		if (in.check(" plotfile ")) { // ps: plotfile fname
			string fname = in.getString();
			ifstream ifs(fname.c_str());
			if (ifs)
				_psi.execute(ifs);
			else
				Message::wstream(true) << "file '" << fname << "' not found in ps: plotfile\n";
		}
		else {
			// ps:<code> is almost identical to ps::[begin]<code> but does
			// a final repositioning to the current DVI location
			executeAndSync(is, true);
			moveToDVIPos();
		}
	}
	return true;
}


/** Handles psfile special.
 *  @param[in] fname EPS file to be included
 *  @param[in] attr attributes given with \\special psfile */
void PsSpecialHandler::psfile (const string &fname, const map<string,string> &attr) {
	EPSFile epsfile(fname);
	istream &is = epsfile.istream();
	if (!is)
		Message::wstream(true) << "file '" << fname << "' not found in special 'psfile'\n";
	else {
		map<string,string>::const_iterator it;

		// bounding box of EPS figure
		double llx = (it = attr.find("llx")) != attr.end() ? str2double(it->second) : 0;
		double lly = (it = attr.find("lly")) != attr.end() ? str2double(it->second) : 0;
		double urx = (it = attr.find("urx")) != attr.end() ? str2double(it->second) : 0;
		double ury = (it = attr.find("ury")) != attr.end() ? str2double(it->second) : 0;

		// desired width/height of resulting figure
		double rwi = (it = attr.find("rwi")) != attr.end() ? str2double(it->second)/10.0 : -1;
		double rhi = (it = attr.find("rhi")) != attr.end() ? str2double(it->second)/10.0 : -1;
		if (rwi == 0 || rhi == 0 || urx-llx == 0 || ury-lly == 0)
			return;

		// user transformations (default values chosen according to dvips manual)
		double hoffset = (it = attr.find("hoffset")) != attr.end() ? str2double(it->second) : 0;
		double voffset = (it = attr.find("voffset")) != attr.end() ? str2double(it->second) : 0;
//		double hsize   = (it = attr.find("hsize")) != attr.end() ? str2double(it->second) : 612;
//		double vsize   = (it = attr.find("vsize")) != attr.end() ? str2double(it->second) : 792;
		double hscale  = (it = attr.find("hscale")) != attr.end() ? str2double(it->second) : 100;
		double vscale  = (it = attr.find("vscale")) != attr.end() ? str2double(it->second) : 100;
		double angle   = (it = attr.find("angle")) != attr.end() ? str2double(it->second) : 0;

		Matrix m(1);
		m.rotate(angle).scale(hscale/100, vscale/100).translate(hoffset, voffset);
		BoundingBox bbox(llx, lly, urx, ury);
		bbox.transform(m);

		double sx = rwi/bbox.width();
		double sy = rhi/bbox.height();
		if (sx < 0)	sx = sy;
		if (sy < 0)	sy = sx;
		if (sx < 0) sx = sy = 1.0;

		// save current DVI position (in pt units)
		const double x = _actions->getX();
		const double y = _actions->getY();

		// all following drawings are relative to (0,0)
		_actions->setX(0);
		_actions->setY(0);
		moveToDVIPos();

		_xmlnode = new XMLElementNode("g");  // append following elements to this group
		_psi.execute("\n@beginspecial @setspecial "); // enter \special environment
		_psi.limit(epsfile.pslength()); // limit the number of bytes to be processed
		_psi.execute(is);               // process EPS file
		_psi.limit(0);                  // disable limitation
		_psi.execute("\n@endspecial "); // leave special environment
		if (_xmlnode->empty())          // nothing been drawn?
			delete _xmlnode;             // => don't need to add empty group node
		else {                          // has anything been drawn?
			Matrix matrix(1);
			matrix.rotate(angle).scale(hscale/100, vscale/100).translate(hoffset, voffset);
			matrix.translate(-llx, lly);
			matrix.scale(sx, sy);      // resize image to width "rwi" and height "rhi"
			matrix.translate(x, y);    // move image to current DVI position
			if (!matrix.isIdentity())
				_xmlnode->addAttribute("transform", matrix.getSVG());
			_actions->appendToPage(_xmlnode);
		}
		_xmlnode = 0;   // append following elements to page group again

		// restore DVI position
		_actions->setX(x);
		_actions->setY(y);
		moveToDVIPos();

		// update bounding box
		m.scale(sx, -sy);
		m.translate(x, y);
		bbox = BoundingBox(0, 0, fabs(urx-llx), fabs(ury-lly));
		bbox.transform(m);
		_actions->embed(bbox);
	}
}


/** Apply transformation to width, height, and depth set by preview package.
 *  @param[in] matrix transformation matrix to apply
 *  @param[out] w width
 *  @param[out] h height
 *  @param[out] d depth
 *  @return true if the baseline is still horizontal after the transformation */
static bool transform_box_extents (const Matrix &matrix, double &w, double &h, double &d) {
	DPair shift = matrix*DPair(0,0);  // the translation component of the matrix
	DPair ex = matrix*DPair(1,0)-shift;
	DPair ey = matrix*DPair(0,1)-shift;
	if (ex.y() != 0 && ey.x() != 0)  // rotation != mod 90 degrees?
		return false;                 // => non-horizontal baseline, can't compute meaningful extents

	if (ex.y() == 0)  // horizontal scaling or skewing?
		w *= fabs(ex.x());
	if (ey.x()==0 || ex.y()==0) { // vertical scaling?
		if (ey.y() < 0) swap(h, d);
		if (double sy = fabs(ey.y())/ey.length()) {
			h *= fabs(ey.y()/sy);
			d *= fabs(ey.y()/sy);
		}
		else
			h = d = 0;
	}
	return true;
}


void PsSpecialHandler::dviEndPage (unsigned) {
	BoundingBox bbox;
	if (_previewFilter.getBoundingBox(bbox)) {
		double w = _previewFilter.width();
		double h = _previewFilter.height();
		double d = _previewFilter.depth();
		bool horiz_baseline = true;
		if (_actions) {
			_actions->bbox() = bbox;
			// apply page transformations to box extents
			Matrix pagetrans;
			_actions->getPageTransform(pagetrans);
			horiz_baseline = transform_box_extents(pagetrans, w, h, d);
			_actions->bbox().lock();
		}
		Message::mstream() << "\napplying bounding box set by preview package (version " << _previewFilter.version() << ")\n";
		if (horiz_baseline) {
			const double bp2pt = 72.27/72.0;
			Message::mstream() <<
				"width=" << XMLString(w*bp2pt) << "pt, "
				"height=" << XMLString(h*bp2pt) << "pt, "
				"depth=" << XMLString(d*bp2pt) << "pt\n";
		}
		else
			Message::mstream() << "can't determine height, width, and depth due to non-horizontal baseline\n";
	}
	// close dictionary TeXDict and execute end-hook if defined
	if (_psSection == PS_BODY) {
		_psi.execute("\nend userdict/end-hook known{end-hook}if ");
		_psSection = PS_HEADERS;
	}
}

///////////////////////////////////////////////////////

void PsSpecialHandler::gsave (vector<double>&) {
	_clipStack.dup();
}


void PsSpecialHandler::grestore (vector<double>&) {
	_clipStack.pop();
}


void PsSpecialHandler::grestoreall (vector<double>&) {
	_clipStack.pop(-1, true);
}


void PsSpecialHandler::save (vector<double> &p) {
	_clipStack.dup(static_cast<int>(p[0]));
}


void PsSpecialHandler::restore (vector<double> &p) {
	_clipStack.pop(static_cast<int>(p[0]));
}


void PsSpecialHandler::moveto (vector<double> &p) {
	_path.moveto(p[0], p[1]);
}


void PsSpecialHandler::lineto (vector<double> &p) {
	_path.lineto(p[0], p[1]);
}


void PsSpecialHandler::curveto (vector<double> &p) {
	_path.cubicto(p[0], p[1], p[2], p[3], p[4], p[5]);
}


void PsSpecialHandler::closepath (vector<double>&) {
	_path.closepath();
}


/** Draws the current path recorded by previously executed path commands (moveto, lineto,...).
 *  @param[in] p not used */
void PsSpecialHandler::stroke (vector<double> &p) {
	_path.removeRedundantCommands();
	if ((_path.empty() && !_clipStack.clippathLoaded()) || !_actions)
		return;

	BoundingBox bbox;
	if (!_actions->getMatrix().isIdentity()) {
		_path.transform(_actions->getMatrix());
		if (!_xmlnode)
			bbox.transform(_actions->getMatrix());
	}
	if (_clipStack.clippathLoaded() && _clipStack.top())
		_path.prepend(*_clipStack.top());
	XMLElementNode *path=0;
	Pair<double> point;
	if (_path.isDot(point)) {  // zero-length path?
		if (_linecap == 1) {    // round line ends?  => draw dot
			double x = point.x();
			double y = point.y();
			double r = _linewidth/2.0;
			path = new XMLElementNode("circle");
			path->addAttribute("cx", x);
			path->addAttribute("cy", y);
			path->addAttribute("r", r);
			path->addAttribute("fill", _actions->getColor().svgColorString());
			bbox = BoundingBox(x-r, y-r, x+r, y+r);
		}
	}
	else {
		// compute bounding box
		_path.computeBBox(bbox);
		bbox.expand(_linewidth/2);

		ostringstream oss;
		_path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
		path = new XMLElementNode("path");
		path->addAttribute("d", oss.str());
		path->addAttribute("stroke", _actions->getColor().svgColorString());
		path->addAttribute("fill", "none");
		if (_linewidth != 1)
			path->addAttribute("stroke-width", _linewidth);
		if (_miterlimit != 4)
			path->addAttribute("stroke-miterlimit", _miterlimit);
		if (_linecap > 0)     // default value is "butt", no need to set it explicitly
			path->addAttribute("stroke-linecap", _linecap == 1 ? "round" : "square");
		if (_linejoin > 0)    // default value is "miter", no need to set it explicitly
			path->addAttribute("stroke-linejoin", _linecap == 1 ? "round" : "bevel");
		if (_opacityalpha < 1)
			path->addAttribute("stroke-opacity", _opacityalpha);
		if (!_dashpattern.empty()) {
			ostringstream oss;
			for (size_t i=0; i < _dashpattern.size(); i++) {
				if (i > 0)
					oss << ',';
				oss << XMLString(_dashpattern[i]);
			}
			path->addAttribute("stroke-dasharray", oss.str());
			if (_dashoffset != 0)
				path->addAttribute("stroke-dashoffset", _dashoffset);
		}
	}
	if (path && _clipStack.top()) {
		// assign clipping path and clip bounding box
		path->addAttribute("clip-path", XMLString("url(#clip")+XMLString(_clipStack.topID())+")");
		BoundingBox clipbox;
		_clipStack.top()->computeBBox(clipbox);
		bbox.intersect(clipbox);
		_clipStack.setClippathLoaded(false);
	}
	if (_xmlnode)
		_xmlnode->append(path);
	else {
		_actions->appendToPage(path);
		_actions->embed(bbox);
	}
	_path.clear();
}


/** Draws a closed path filled with the current color.
 *  @param[in] p not used
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::fill (vector<double> &p, bool evenodd) {
	_path.removeRedundantCommands();
	if ((_path.empty() && !_clipStack.clippathLoaded()) || !_actions)
		return;

	// compute bounding box
	BoundingBox bbox;
	_path.computeBBox(bbox);
	if (!_actions->getMatrix().isIdentity()) {
		_path.transform(_actions->getMatrix());
		if (!_xmlnode)
			bbox.transform(_actions->getMatrix());
	}
	if (_clipStack.clippathLoaded() && _clipStack.top())
		_path.prepend(*_clipStack.top());

	ostringstream oss;
	_path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	XMLElementNode *path = new XMLElementNode("path");
	path->addAttribute("d", oss.str());
	if (_pattern)
		path->addAttribute("fill", XMLString("url(#")+_pattern->svgID()+")");
	else if (_actions->getColor() != Color::BLACK || _savenode)
		path->addAttribute("fill", _actions->getColor().svgColorString());
	if (_clipStack.top()) {
		// assign clipping path and clip bounding box
		path->addAttribute("clip-path", XMLString("url(#clip")+XMLString(_clipStack.topID())+")");
		BoundingBox clipbox;
		_clipStack.top()->computeBBox(clipbox);
		bbox.intersect(clipbox);
		_clipStack.setClippathLoaded(false);
	}
	if (evenodd)  // SVG default fill rule is "nonzero" algorithm
		path->addAttribute("fill-rule", "evenodd");
	if (_opacityalpha < 1)
		path->addAttribute("fill-opacity", _opacityalpha);
	if (_xmlnode)
		_xmlnode->append(path);
	else {
		_actions->appendToPage(path);
		_actions->embed(bbox);
	}
	_path.clear();
}


/** Creates a Matrix object out of a given sequence of 6 double values.
 *  The given values must be arranged in PostScript matrix order.
 *  @param[in] v vector containing the matrix values
 *  @param[in] startindex vector index of first component
 *  @param[out] matrix the generated matrix */
static void create_matrix (vector<double> &v, int startindex, Matrix &matrix) {
	// Ensure vector p has 6 elements. If necessary, add missing ones
	// using corresponding values of the identity matrix.
	if (v.size()-startindex < 6) {
		v.resize(6+startindex);
		for (int i=v.size()-startindex; i < 6; ++i)
			v[i+startindex] = (i%3 ? 0 : 1);
	}
	// PS matrix [a b c d e f] equals ((a,b,0),(c,d,0),(e,f,1)).
	// Since PS uses left multiplications, we must transpose and reorder
	// the matrix to ((a,c,e),(b,d,f),(0,0,1)). This is done by the
	// following swaps.
	swap(v[startindex+1], v[startindex+2]);  // => (a, c, b, d, e, f)
	swap(v[startindex+2], v[startindex+4]);  // => (a, c, e, d, b, f)
	swap(v[startindex+3], v[startindex+4]);  // => (a, c, e, b, d, f)
	matrix.set(v, startindex);
}


/** Starts the definition of a new fill pattern. This operator
 *  expects 9 parameters for tiling patterns (see PS reference 4.9.2):
 *  @param[in] p the 9 values defining a tiling pattern (see PS reference 4.9.2):
 *  0: pattern type (0:none, 1:tiling, 2:shading)
 *  1: pattern ID
 *  2-5: lower left and upper right coordinates of pattern box
 *  6: horizontal distance of adjacent tiles
 *  7: vertical distance of adjacent tiles
 *  8: paint type (1: colored pattern, 2: uncolored pattern)
 *  9-14: pattern matrix */
void PsSpecialHandler::makepattern (vector<double> &p) {
	int pattern_type = static_cast<int>(p[0]);
	int id = static_cast<int>(p[1]);
	switch (pattern_type) {
		case 0:
			// pattern definition completed
			if (_savenode) {
				_xmlnode = _savenode;
				_savenode = 0;
			}
			break;
		case 1: {  // tiling pattern
			BoundingBox bbox(p[2], p[3], p[4], p[5]);
			const double &xstep=p[6], &ystep=p[7]; // horizontal and vertical distance of adjacent tiles
			int paint_type = static_cast<int>(p[8]);

			Matrix matrix;  // transformation matrix given together with pattern definition
			create_matrix(p, 9, matrix);
			matrix.rmultiply(_actions->getMatrix());

			PSTilingPattern *pattern=0;
			if (paint_type == 1)
				pattern = new PSColoredTilingPattern(id, bbox, matrix, xstep, ystep);
			else
				pattern = new PSUncoloredTilingPattern(id, bbox, matrix, xstep, ystep);
			_patterns[id] = pattern;
			_savenode = _xmlnode;
			_xmlnode = pattern->getContainerNode();  // insert the following SVG elements into this node
			break;
		}
		case 2: {
			// define a shading pattern
		}
	}
}


/** Selects a previously defined fill pattern.
 *  0: pattern ID
 *  1-3: (optional) RGB values for uncolored tiling patterns
 *  further parameters depend on the pattern type */
void PsSpecialHandler::setpattern (vector<double> &p) {
	int pattern_id = p[0];
	Color color;
	if (p.size() == 4)
		color.setRGB(p[1], p[2], p[3]);
	map<int,PSPattern*>::iterator it = _patterns.find(pattern_id);
	if (it == _patterns.end())
		_pattern = 0;
	else {
		if (PSUncoloredTilingPattern *pattern = dynamic_cast<PSUncoloredTilingPattern*>(it->second))
			pattern->setColor(color);
		it->second->apply(_actions);
		if (PSTilingPattern *pattern = dynamic_cast<PSTilingPattern*>(it->second))
			_pattern = pattern;
		else
			_pattern = 0;
	}
}


/** Clears the current clipping path.
 *  @param[in] p not used */
void PsSpecialHandler::initclip (vector<double> &) {
	_clipStack.pushEmptyPath();
}


/** Assigns the current clipping path to the graphics path. */
void PsSpecialHandler::clippath (std::vector<double>&) {
	if (!_clipStack.empty()) {
		_clipStack.setClippathLoaded(true);
		_path.clear();
	}
}


/** Assigns a new clipping path to the graphics state using the current path.
 *  If the graphics state already contains a clipping path, the new one is
 *  computed by intersecting the current clipping path with the current graphics
 *  path (see PS language reference, 3rd edition, pp. 193, 542)
 *  @param[in] p not used
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::clip (vector<double>&, bool evenodd) {
	clip(_path, evenodd);
}


/** Assigns a new clipping path to the graphics state using the current path.
 *  If the graphics state already contains a clipping path, the new one is
 *  computed by intersecting the current one with the given path.
 *  @param[in] path path used to restrict the clipping region
 *  @param[in] evenodd true: use even-odd fill algorithm, false: use nonzero fill algorithm */
void PsSpecialHandler::clip (Path &path, bool evenodd) {
	// when this method is called, _path contains the clipping path
	_path.removeRedundantCommands();
	if (path.empty() || !_actions)
		return;

	Path::WindingRule windingRule = evenodd ? Path::WR_EVEN_ODD : Path::WR_NON_ZERO;
	path.setWindingRule(windingRule);

	if (!_actions->getMatrix().isIdentity())
		path.transform(_actions->getMatrix());

	int oldID = _clipStack.topID();

	ostringstream oss;
	if (!COMPUTE_CLIPPATHS_INTERSECTIONS || oldID < 1) {
		_clipStack.replace(path);
		path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	}
	else {
		// compute the intersection of the current clipping path with the current graphics path
		Path *oldPath = _clipStack.getPath(oldID);
		Path intersectedPath(windingRule);
		PathClipper clipper;
		clipper.intersect(*oldPath, path, intersectedPath);
		_clipStack.replace(intersectedPath);
		intersectedPath.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
	}

	XMLElementNode *pathElem = new XMLElementNode("path");
	pathElem->addAttribute("d", oss.str());
	if (evenodd)
		pathElem->addAttribute("clip-rule", "evenodd");

	int newID = _clipStack.topID();
	XMLElementNode *clipElem = new XMLElementNode("clipPath");
	clipElem->addAttribute("id", XMLString("clip")+XMLString(newID));
	if (!COMPUTE_CLIPPATHS_INTERSECTIONS && oldID)
		clipElem->addAttribute("clip-path", XMLString("url(#clip")+XMLString(oldID)+")");

	clipElem->append(pathElem);
	_actions->appendToDefs(clipElem);
}


/** Applies a gradient fill to the current graphics path. Vector p contains the shading parameters
 *  in the following order:
 *  - shading type (6=Coons, 7=tensor product)
 *  - color space (1=gray, 3=rgb, 4=cmyk)
 *  - 1.0 followed by the background color components based on the declared color space, or 0.0
 *  - 1.0 followed by the bounding box coordinates, or 0.0
 *  - geometry and color parameters depending on the shading type */
void PsSpecialHandler::shfill (vector<double> &params) {
	if (params.size() < 9)
		return;

	// collect common data relevant for all shading types
	int shadingTypeID = static_cast<int>(params[0]);
	ColorSpace colorSpace = Color::RGB_SPACE;
	switch (static_cast<int>(params[1])) {
		case 1: colorSpace = Color::GRAY_SPACE; break;
		case 3: colorSpace = Color::RGB_SPACE; break;
		case 4: colorSpace = Color::CMYK_SPACE; break;
	}
	VectorIterator<double> it = params;
	it += 2;     // skip shading type and color space
	// Get color to fill the whole mesh area before drawing the gradient colors on top of that background.
	// This is an optional parameter to shfill.
	bool bgcolorGiven = static_cast<bool>(*it++);
	Color bgcolor;
	if (bgcolorGiven)
		bgcolor.set(colorSpace, it);
	// Get clipping rectangle to limit the drawing area of the gradient mesh.
	// This is an optional parameter to shfill too.
	bool bboxGiven = static_cast<bool>(*it++);
	if (bboxGiven) { // bounding box given
		Path bboxPath;
		const double &x1 = *it++;
		const double &y1 = *it++;
		const double &x2 = *it++;
		const double &y2 = *it++;
		bboxPath.moveto(x1, y1);
		bboxPath.lineto(x2, y1);
		bboxPath.lineto(x2, y2);
		bboxPath.lineto(x1, y2);
		bboxPath.closepath();
		clip(bboxPath, false);
	}
	try {
		if (shadingTypeID == 5)
			processLatticeTriangularPatchMesh(colorSpace, it);
		else
			processSequentialPatchMesh(shadingTypeID, colorSpace, it);
	}
	catch (ShadingException &e) {
		Message::estream(false) << "PostScript error: " << e.what() << '\n';
		it.invalidate();  // stop processing the remaining patch data
	}
	catch (IteratorException &e) {
		Message::estream(false) << "PostScript error: incomplete shading data\n";
	}
	if (bboxGiven)
		_clipStack.pop();
}


/** Reads position and color data of a single shading patch from the data vector.
 *  @param[in] shadingTypeID PS shading type ID identifying the format of the subsequent patch data
 *  @param[in] edgeflag edge flag specifying how to connect the current patch to the preceding one
 *  @param[in] cspace color space used to compute the color gradient
 *  @param[in,out] it iterator used to sequentially access the patch data
 *  @param[out] points the points defining the geometry of the patch
 *  @param[out] colors the colors assigned to the vertices of the patch */
static void read_patch_data (ShadingPatch &patch, int edgeflag,
		VectorIterator<double> &it, vector<DPair> &points, vector<Color> &colors)
{
	// number of control points and colors required to define a single patch
	int numPoints = patch.numPoints(edgeflag);
	int numColors = patch.numColors(edgeflag);
	points.resize(numPoints);
	colors.resize(numColors);
	if (patch.psShadingType() == 4) {
		// format of a free-form triangular patch definition, where eN denotes
		// the edge of the corresponding vertex:
		// edge flag = 0, x1, y1, {color1}, e2, x2, y2, {color2}, e3, x3, y3, {color3}
		// edge flag > 0, x1, y1, {color1}
		for (int i=0; i < numPoints; i++) {
			if (i > 0) ++it;  // skip redundant edge flag from free-form triangular patch
			double x = *it++;
			double y = *it++;
			points[i] = DPair(x, y);
			colors[i].set(patch.colorSpace(), it);
		}
	}
	else if (patch.psShadingType() == 6 || patch.psShadingType() == 7) {
		// format of each Coons/tensor product patch definition:
		// edge flag = 0, x1, y1, ... , xn, yn, {color1}, {color2}, {color3}, {color4}
		// edge flag > 0, x5, y5, ... , xn, yn, {color3}, {color4}
		for (int i=0; i < numPoints; i++) {
			double x = *it++;
			double y = *it++;
			points[i] = DPair(x, y);
		}
		for (int i=0; i < numColors; i++)
			colors[i].set(patch.colorSpace(), it);
	}
}


class ShadingCallback : public ShadingPatch::Callback {
	public:
		ShadingCallback (SpecialActions *actions, XMLElementNode *parent, int clippathID)
			: _actions(actions), _group(new XMLElementNode("g"))
		{
			if (parent)
				parent->append(_group);
			else
				actions->appendToPage(_group);
			if (clippathID > 0)
				_group->addAttribute("clip-path", XMLString("url(#clip")+XMLString(clippathID)+")");
		}

		void patchSegment (GraphicsPath<double> &path, const Color &color) {
			if (!_actions->getMatrix().isIdentity())
				path.transform(_actions->getMatrix());

			// draw a single patch segment
			ostringstream oss;
			path.writeSVG(oss, SVGTree::RELATIVE_PATH_CMDS);
			XMLElementNode *pathElem = new XMLElementNode("path");
			pathElem->addAttribute("d", oss.str());
			pathElem->addAttribute("fill", color.svgColorString());
			_group->append(pathElem);
		}

	private:
		SpecialActions *_actions;
		XMLElementNode *_group;
};


/** Handle all patch meshes whose patches and their connections can be processed sequentially.
 *  This comprises free-form triangular, Coons, and tensor-product patch meshes. */
void PsSpecialHandler::processSequentialPatchMesh (int shadingTypeID, ColorSpace colorSpace, VectorIterator<double> &it) {
	auto_ptr<ShadingPatch> previousPatch;
	while (it.valid()) {
		int edgeflag = static_cast<int>(*it++);
		vector<DPair> points;
		vector<Color> colors;
		auto_ptr<ShadingPatch> patch;

		patch = auto_ptr<ShadingPatch>(ShadingPatch::create(shadingTypeID, colorSpace));
		read_patch_data(*patch, edgeflag, it, points, colors);
		patch->setPoints(points, edgeflag, previousPatch.get());
		patch->setColors(colors, edgeflag, previousPatch.get());
		ShadingCallback callback(_actions, _xmlnode, _clipStack.topID());
#if 0
		if (bgcolorGiven) {
			// fill whole patch area with given background color
			GraphicsPath<double> outline;
			patch->getBoundaryPath(outline);
			callback.patchSegment(outline, bgcolor);
		}
#endif
		patch->approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);
		if (!_xmlnode) {
			// update bounding box
			BoundingBox bbox;
			patch->getBBox(bbox);
			bbox.transform(_actions->getMatrix());
			_actions->embed(bbox);
		}
		previousPatch = patch;
	}
}


struct PatchVertex {
	DPair point;
	Color color;
};


void PsSpecialHandler::processLatticeTriangularPatchMesh (ColorSpace colorSpace, VectorIterator<double> &it) {
	int verticesPerRow = static_cast<int>(*it++);
	if (verticesPerRow < 2)
		return;

	// hold two adjacent rows of vertices and colors
	vector<PatchVertex> row1(verticesPerRow);
	vector<PatchVertex> row2(verticesPerRow);
	vector<PatchVertex> *rowptr1 = &row1;
	vector<PatchVertex> *rowptr2 = &row2;
	// read data of first row
	for (int i=0; i < verticesPerRow; i++) {
		PatchVertex &vertex = (*rowptr1)[i];
		vertex.point.x(*it++);
		vertex.point.y(*it++);
		vertex.color.set(colorSpace, it);
	}
	LatticeTriangularPatch patch(colorSpace);
	ShadingCallback callback(_actions, _xmlnode, _clipStack.topID());
	while (it.valid()) {
		// read next row
		for (int i=0; i < verticesPerRow; i++) {
			PatchVertex &vertex = (*rowptr2)[i];
			vertex.point.x(*it++);
			vertex.point.y(*it++);
			vertex.color.set(colorSpace, it);
		}
		// create triangular patches for the vertices of the two rows
		for (int i=0; i < verticesPerRow-1; i++) {
			const PatchVertex &v1 = (*rowptr1)[i], &v2 = (*rowptr1)[i+1];
			const PatchVertex &v3 = (*rowptr2)[i], &v4 = (*rowptr2)[i+1];
			patch.setPoints(v1.point, v2.point, v3.point);
			patch.setColors(v1.color, v2.color, v3.color);
			patch.approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);

			patch.setPoints(v2.point, v3.point, v4.point);
			patch.setColors(v2.color, v3.color, v4.color);
			patch.approximate(SHADING_SEGMENT_SIZE, SHADING_SEGMENT_OVERLAP, SHADING_SIMPLIFY_DELTA, callback);
		}
		swap(rowptr1, rowptr2);
	}
}


/** Clears current path */
void PsSpecialHandler::newpath (vector<double> &p) {
	bool drawing = (p[0] > 0);
	if (!drawing || !_clipStack.clippathLoaded()) {
		_path.clear();
		_clipStack.setClippathLoaded(false);
	}
}


void PsSpecialHandler::setmatrix (vector<double> &p) {
	if (_actions) {
		Matrix m;
		create_matrix(p, 0, m);
		_actions->setMatrix(m);
	}
}


// In contrast to SVG, PostScript transformations are applied in
// reverse order (M' = T*M). Thus, the transformation matrices must be
// left-multiplied in the following methods scale(), translate() and rotate().


void PsSpecialHandler::scale (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		ScalingMatrix s(p[0], p[1]);
		m.lmultiply(s);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::translate (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		TranslationMatrix t(p[0], p[1]);
		m.lmultiply(t);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::rotate (vector<double> &p) {
	if (_actions) {
		Matrix m = _actions->getMatrix();
		RotationMatrix r(p[0]);
		m.lmultiply(r);
		_actions->setMatrix(m);
	}
}


void PsSpecialHandler::setgray (vector<double> &p) {
	_pattern = 0;
	_currentcolor.setGray(p[0]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::setrgbcolor (vector<double> &p) {
	_pattern= 0;
	_currentcolor.setRGB(p[0], p[1], p[2]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::setcmykcolor (vector<double> &p) {
	_pattern = 0;
	_currentcolor.setCMYK(p[0], p[1], p[2], p[3]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


void PsSpecialHandler::sethsbcolor (vector<double> &p) {
	_pattern = 0;
	_currentcolor.setHSB(p[0], p[1], p[2]);
	if (_actions)
		_actions->setColor(_currentcolor);
}


/** Sets the dash parameters used for stroking.
 *  @param[in] p dash pattern array m1,...,mn plus trailing dash offset */
void PsSpecialHandler::setdash (vector<double> &p) {
	_dashpattern.clear();
	for (size_t i=0; i < p.size()-1; i++)
		_dashpattern.push_back(scale(p[i]));
	_dashoffset = scale(p.back());
}


/** This method is called by the PSInterpreter if an PS operator has been executed. */
void PsSpecialHandler::executed () {
	if (_actions)
		_actions->progress("ps");
}

////////////////////////////////////////////

void PsSpecialHandler::ClippingStack::pushEmptyPath () {
	if (!_stack.empty())
		_stack.push(Entry(0, -1));
}


void PsSpecialHandler::ClippingStack::push (const Path &path, int saveID) {
	if (path.empty())
		_stack.push(Entry(0, saveID));
	else {
		_paths.push_back(path);
		_stack.push(Entry(_paths.size(), saveID));
	}
}


/** Pops a single or several elements from the clipping stack.
 *  The method distingushes between the following cases:
 *  1) saveID < 0 and grestoreall == false:
 *     pop top element if it was pushed by gsave (its saveID is < 0 as well)
 *  2) saveID < 0 and grestoreall == true
 *     repeat popping until stack is empty or the top element was pushed
 *     by save (its saveID is >= 0)
 *  3) saveID >= 0:
 *     pop all elements until the saveID of the top element equals parameter saveID */
void PsSpecialHandler::ClippingStack::pop (int saveID, bool grestoreall) {
	if (!_stack.empty()) {
		if (saveID < 0) {                // grestore?
			if (_stack.top().saveID < 0)  // pushed by 'gsave'?
				_stack.pop();
			// pop all further elements pushed by 'gsave' if grestoreall == true
			while (grestoreall && !_stack.empty() && _stack.top().saveID < 0)
				_stack.pop();
		}
		else {
			// pop elements pushed by 'gsave'
			while (!_stack.empty() && _stack.top().saveID != saveID)
				_stack.pop();
			// pop element pushed by 'save'
			if (!_stack.empty())
				_stack.pop();
		}
	}
}


/** Returns a pointer to the path on top of the stack, or 0 if the stack is empty. */
const PsSpecialHandler::Path* PsSpecialHandler::ClippingStack::top () const {
	return (!_stack.empty() && _stack.top().pathID)
		? &_paths[_stack.top().pathID-1]
		: 0;
}


PsSpecialHandler::Path* PsSpecialHandler::ClippingStack::getPath (size_t id) {
	return (id > 0 && id <= _paths.size()) ? &_paths[id-1] : 0;
}


/** Returns true if the clipping path was loaded into the graphics path (via PS operator 'clippath') */
bool PsSpecialHandler::ClippingStack::clippathLoaded () const {
	return !_stack.empty() && _stack.top().cpathLoaded;
}


void PsSpecialHandler::ClippingStack::setClippathLoaded (bool loaded) {
	if (_stack.empty())
		return;
	_stack.top().cpathLoaded = loaded;
}


/** Pops all elements from the stack. */
void PsSpecialHandler::ClippingStack::clear() {
	_paths.clear();
	while (!_stack.empty())
		_stack.pop();
}


/** Replaces the top element by a new one.
 *  @param[in] path new path to be on top of the stack */
void PsSpecialHandler::ClippingStack::replace (const Path &path) {
	if (_stack.empty())
		push(path, -1);
	else {
		_paths.push_back(path);
		_stack.top().pathID = _paths.size();
	}
}


/** Duplicates the top element, i.e. the top element is pushed again. */
void PsSpecialHandler::ClippingStack::dup (int saveID) {
	_stack.push(_stack.empty() ? Entry(0, -1) : _stack.top());
	_stack.top().saveID = saveID;
}


const char** PsSpecialHandler::prefixes () const {
	static const char *pfx[] = {"header=", "psfile=", "PSfile=", "ps:", "ps::", "!", "\"", 0};
	return pfx;
}
