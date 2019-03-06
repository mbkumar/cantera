

#include "cantera/thermo/LateralInteraction.h"
#include "cantera/thermo/Species.h"
//#include "cantera/thermo/LatIntThermoFactory.h"
#include "cantera/base/stringUtils.h"
#include "cantera/base/ctexceptions.h"
#include "cantera/base/ctml.h"
#include <iostream>
#include <limits>
#include <vector>
#include <string>
#include <utility>


namespace Cantera {

LateralInteraction::LateralInteraction()
{
}


LateralInteraction::LateralInteraction(std::string species1, std::string species2,
                                       const vector_fp& strengths,   
                                       const vector_fp& intercepts, 
                                       std::string name) :
    m_strengths(strengths), m_cov_thresholds(intercepts), m_id(name)
{
    m_species = make_pair(species1, species2);
}


LateralInteraction::~LateralInteraction()
{
}

bool LateralInteraction::validate()
{
    if (m_strengths.size() == m_cov_thresholds.size()+1)
        return true;
    else
        return false;
}


std::string LateralInteraction::species1Name() { 
    //return m_species.first->name; 
    return m_species.first; 
}

std::string LateralInteraction::species2Name() { 
    //return m_species.second->name; 
    return m_species.second; 
}

double LateralInteraction::strength(const double coverage) const {
    /*
    if (coverage > 1) {
        throw CanteraError ("Coverage cannot be greater than 1");
    }
    else {
        if (coverage < 0) {
            throw CanteraError ("Coverage cannot be less than 0");
        }
    }*/

    doublereal val = 0.0;
    for (auto i=0; i < m_strengths.size(); i++) {
        auto cov_low_thr = m_cov_thresholds[i];
        auto cov_up_thr = m_cov_thresholds[i+1];
        if (cov_up_thr < coverage) {
            val += (cov_up_thr - cov_low_thr) * m_strengths[i];
        } 
        else {
            val += (coverage - cov_low_thr) * m_strengths[i];
            break;
        }
    }

    return  val;
}



shared_ptr<LateralInteraction> newLateralInteraction(const XML_Node& interaction_node)
{
    //if (interaction_node.hasAttrib("id")){
    std::string id = interaction_node["id"];
    //}
    const XML_Node& sp_array = interaction_node.child("speciesArray");
    std::vector<std::string> species;
    getStringArray(sp_array, species);
    if (species.size() != 2)
        throw CanteraError("Cantera::newLateralInteraction", 
                "The size of the species array: '{}' is different from 2",  
                species.size());
                           //+ sp_array["datasrc"]);
    //XML_Node* db = get_XML_Node(sp_array["datasrc"], &interaction_node.root());
    /*if (db == 0) {
        throw CanteraError(" Can not find the XML node for species databases: ");
                           //+ sp_array["datasrc"]);
    }*/
    
    std::vector<XML_Node*> fas = interaction_node.getChildren("floatArray");
    vector_fp strengths, cov_thresholds;
    for (auto fa: fas){
        if (fa->name() == "strength")
            getFloatArray(*fa, strengths);//, nodeName = "strength")
        if (fa->name() == "coverage_threshold")
            getFloatArray(*fa, cov_thresholds);//, nodeName = "coverage_threshold")
    }

    
    auto interaction = make_shared<LateralInteraction>(species[0], species[1], 
                                                       strengths, cov_thresholds, id);
    //auto interaction = make_shared<LateralInteraction>();

    return interaction;
}


std::vector<shared_ptr<LateralInteraction> > getInteractions(const XML_Node& node)
{
    std::vector<shared_ptr<LateralInteraction> > interactions;
    for (const auto& intnode : node.child("interactionData").getChildren("interaction")) {
        interactions.push_back(newLateralInteraction(*intnode));
    }
    return interactions;
}

}
