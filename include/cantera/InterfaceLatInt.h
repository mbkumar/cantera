/**
 * @file InterfaceLatInt.h
 *   Declaration and Definition for the class Interface.
 */

// This file is part of Cantera. See License.txt in the top-level directory or
// at http://www.cantera.org/license.txt for license and copyright information.

#ifndef CXX_INTERFACELATINT
#define CXX_INTERFACELATINT

#include "thermo.h"
#include "kinetics.h"
#include "cantera/thermo/SurfLatIntPhase.h"
#include "cantera/kinetics/InterfaceKinetics.h"

namespace Cantera
{
//! An interface between multiple bulk phases.
/*!
 * This class is defined mostly for convenience. It inherits both from SurfPhase
 * and InterfaceKinetics. It therefore represents a surface phase, and also acts
 * as the kinetics manager to manage reactions occurring on the surface,
 * possibly involving species from other phases.
 */
class InterfaceInteractions :
    public SurfLatIntPhase,
    public InterfaceKinetics
{
public:
    //! Constructor.
    /*!
     * Construct an Interface instance from a specification in an input file.
     *
     * @param infile  Cantera input file in CTI or CTML format.
     * @param id      Identification string to distinguish between multiple
     *     definitions within one input file.
     * @param otherPhases  Neighboring phases that may participate in the
     *     reactions on this interface. Don't include the surface phase
     */
    InterfaceInteractions(const std::string& infile, std::string id,
              std::vector<ThermoPhase*> otherPhases) :
        m_ok(false),
        m_r(0) {
        m_r = get_XML_File(infile);
        if (id == "-") {
            id = "";
        }

        XML_Node* x = get_XML_Node("#"+id, m_r);
        if (!x) {
            throw CanteraError("Interface","error in get_XML_Node");
        }
        importPhase(*x, this);
        otherPhases.push_back(this);
        if (nInteractions()) {
            m_has_thermo_coverage_dependence = true;  // Make this true only if lateral interactions are present. 
        }
        importKinetics(*x, otherPhases, this);
        m_ok = true;
    }

    //! Not operator
    bool operator!() {
        return !m_ok;
    }

    //! return whether the object has been instantiated
    bool ready() const {
        return m_ok;
    }

protected:
    //! Flag indicating that the object has been instantiated
    bool m_ok;

    //! XML_Node pointer to the XML File object that contains the Surface and
    //! the Interfacial Reaction object description
    XML_Node* m_r;
};

//! Import an instance of class Interface from a specification in an input file.
/*!
 *  This is the preferred method to create an InterfaceInteractions instance.
 */
inline InterfaceInteractions* importInterfaceInteractions(
            const std::string& infile, 
            const std::string& id, 
            std::vector<ThermoPhase*> phases)
{
    return new InterfaceInteractions(infile, id, phases);
}

}

#endif
