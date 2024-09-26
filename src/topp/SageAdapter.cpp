// Copyright (c) 2002-present, The OpenMS Team -- EKU Tuebingen, ETH Zurich, and FU Berlin
// SPDX-License-Identifier: BSD-3-Clause
//
// --------------------------------------------------------------------------
// $Maintainer: Timo Sachsenberg $
// $Authors: Timo Sachsenberg $
// --------------------------------------------------------------------------

#include <OpenMS/APPLICATIONS/SearchEngineBase.h>

#include <OpenMS/ANALYSIS/ID/PeptideIndexing.h>
#include <OpenMS/DATASTRUCTURES/DefaultParamHandler.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/FORMAT/PepXMLFile.h>
#include <OpenMS/FORMAT/IdXMLFile.h>
#include <OpenMS/FORMAT/ControlledVocabulary.h>
#include <OpenMS/FORMAT/PercolatorInfile.h>
#include <OpenMS/FORMAT/HANDLERS/IndexedMzMLDecoder.h>
#include <OpenMS/FORMAT/DATAACCESS/MSDataWritingConsumer.h>
#include <OpenMS/CHEMISTRY/ModificationsDB.h>
#include <OpenMS/CHEMISTRY/ProteaseDB.h>
#include <OpenMS/CHEMISTRY/ResidueDB.h>
#include <OpenMS/CHEMISTRY/ResidueModification.h>
#include <OpenMS/CHEMISTRY/ModifiedPeptideGenerator.h>
#include <OpenMS/PROCESSING/ID/IDFilter.h>

#include <OpenMS/SYSTEM/File.h>

#include <fstream>
#include <regex>

#include <QStringList>
#include <chrono>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

#include <boost/math/distributions/normal.hpp>

using namespace OpenMS;
using namespace std;
using boost::math::normal;

//-------------------------------------------------------------
//Doxygen docu
//-------------------------------------------------------------

/**
@page TOPP_SageAdapter SageAdapter

@brief Identifies peptides in MS/MS spectra via sage.

<CENTER>
    <table>
        <tr>
            <th ALIGN = "center"> pot. predecessor tools </td>
            <td VALIGN="middle" ROWSPAN=2> &rarr; SageAdapter &rarr;</td>
            <th ALIGN = "center"> pot. successor tools </td>
        </tr>
        <tr>
            <td VALIGN="middle" ALIGN = "center" ROWSPAN=1> any signal-/preprocessing tool @n (in mzML format)</td>
            <td VALIGN="middle" ALIGN = "center" ROWSPAN=1> @ref TOPP_IDFilter or @n any protein/peptide processing tool</td>
        </tr>
    </table>
</CENTER>

@em Sage must be installed before this wrapper can be used.

Only the closed-search identification mode of Sage is supported by this adapter.
Currently, also neither "wide window" (= open or DIA) mode, nor "chimeric" mode is supported,
because of limitations in OpenMS' data structures and file formats.

<B>The command line parameters of this tool are:</B>
@verbinclude TOPP_SageAdapter.cli
<B>INI file documentation of this tool:</B>
@htmlinclude TOPP_SageAdapter.html
*/

// We do not want this class to show up in the docu:
/// @cond TOPPCLASSES

#define CHRONOSET

class TOPPSageAdapter :
  public SearchEngineBase
{
public: 
  TOPPSageAdapter() :
    SearchEngineBase("SageAdapter", "Annotates MS/MS spectra using Sage.", true,
             {
                 {"Michael Lazear",
                 "Sage: An Open-Source Tool for Fast Proteomics Searching and Quantification at Scale",
                 "J. Proteome Res. 2023, 22, 11, 3652–3659",
                 "https://doi.org/10.1021/acs.jproteome.3c00486"}
             })
  {
  }

  // Saves details of PTMs as well, useful for if more than one PTM is mapped to a given mass 
  struct modification
  {
    double rate = 0; 
    vector<double> mass; 
    double numcharges = 0; 
  }; 

  // Comparator for approximate comparison of double values
  struct FuzzyDoubleComparator {
      double epsilon;
      FuzzyDoubleComparator(double eps = 1e-9) : epsilon(eps) {}
      bool operator()(const double& a, const double& b) const 
      {
          return std::fabs(a - b) >= epsilon && a < b;
      }
  };

// delta mass counts to delta masses
//typedef map<double, double, FuzzyDoubleComparator> CountToDeltaMass;

typedef map<double, double, FuzzyDoubleComparator> DeltaMassHistogram; // maps delta mass to count

// Gaussian function
static double gaussian(double x, double sigma) {
    return exp(-(x*x) / (2 * sigma*sigma)) / (sigma * sqrt(2 * M_PI));
}

// Smooths the delta mass histogram using a Kernel Density Estimation. 
static DeltaMassHistogram smoothDeltaMassHist(const DeltaMassHistogram& hist, double sigma = 0.001) 
{
    std::map<double, double, FuzzyDoubleComparator> smoothed_hist(FuzzyDoubleComparator(1e-9));
    std::vector<double> delta, count;

    // Extract delta masses and counts
    for (const auto& pair : hist) 
    {
      deltas.push_back(pair.first);
      counts.push_back(pair.second);
    }

    // Apply Gaussian smoothing
    for (size_t i = 0; i < deltas.size(); ++i) 
    {
      double smoothed_counts = 0.0;
      double weightSum = 0.0;

      for (size_t j = 0; j < delta.size(); ++j) 
      {
        double mzDiff = std::abs(deltas[i] - deltas[j]);
        if (mzDiff > 3.0 * sigma) continue; // Ignore points too far away
        double weight = gaussian(mzDiff, sigma);
        smoothed_hist += weight * counts[j];
        weightSum += weight;
      }

      smoothed_hist[deltas[i]] = smoothed_counts / weightSum;
    }

    return smoothed_hist;
}

// Picks local maxima in a PTM-mass histogram
static DeltaMassHistogram findPeaksInDeltaMassHistogram(const DeltaMassHistogram& hist, double count_threshold = 0.0, double SNR = 2.0) 
{
  if (hist.size() < 3) 
  {
    return hist;  // not enough data points, return original histogram
  }

  DeltaMassHistogram smoothed_hist;

  // Calculate noise level (e.g., median count)
  std::vector<double> counts;
  for (const auto& pair : hist) 
  {
    counts.push_back(pair.second);
  }

  size_t n = counts.size() / 2;
  std::nth_element(counts.begin(), counts.begin() + n, counts.end());
  double noiseLevel = counts[n];

  auto it = hist.begin();
  auto prev = it++;
  auto next = std::next(it);

  while (next != hist.end()) 
  {
    // Check if current point is a local maximum
    if ( !(it->second < prev->second) && !(it->second < next->second) && 
      it->second > count_threshold &&
      it->second / noiseLevel > SNR)   
    {
      smoothed_hist[it->first] = it->second; 
    }
    prev = it;
    it = next;
    ++next;
  }

  return smoothed_hist;
}

// Function that returns the maxima of a histogram out of the delta-masses of each peptide, takes a vector of peptide-ids 
// TODO: create map from delta mass to charge states
pair< vector<double>, pair<DeltaMassHistogram, DeltaMassToCharges>> getDeltaClusterCenter(const vector<PeptideIdentification>& pips, bool smoothing = false,  bool debug = false)
{
  // Data structures to store the data from Sage 
  vector<int> delta_masses, charges;

  std::vector<double> sorted_keys;  // vector to maintain sorted keys

  for (auto& id : pips)
  {
    auto& hits = id.getHits();
    for (auto& h : hits)
    {
      int charge = h.getCharge();
      double deltamass = expval - calcval; // TODO: check with PercolatorInfile and read correct metavalue
      delta_masses.push_back(deltamass);
      charges.push_back(charge);
    }
  }



      // Perform binary search on the sorted keys
      auto it = std::lower_bound(sorted_keys.begin(), sorted_keys.end(), deltamass);

      // border cases
      if (it == sorted_keys.begin())
      {
        return 0; // index 0 
      }
      if (it == sorted_keys.end())
      {
        return sorted_keys.size()- 1; // index of last element
      }

      // the entry before or the current entry are closest
      ConstIterator before = it;
      --before;
      if (std::fabs(it->first - deltamass) < std::fabs(before->first - deltamass))
      {
        return Size(it - ContainerType::begin()); // index of it
      }
      else
      {
        return Size(it2 - ContainerType::begin()); // index of before
      }

      const double found_deltamass = sorted_keys[found_index];
      if (found_deltamass >= deltamass - tolerance && found_deltamass <= deltamass + tolerance)
      { // Mass shift is already in histogram  
        return static_cast<Int>(i); // index of found_index
        hist_it->second += 1.0;

        auto& charges = charge_states[deltamass];
        if (std::find(charges.begin(), charges.end(), charge) == charges.end())
        {
          num_charges_at_mass[deltamass] += 1.0;
          charges.push_back(charge);
        }

      }
      else
      {
        // create new bucket
      }      

      if (hist_it == hist.end()) // exact mass shift isn't in histogram but maybe a similar one
      {
          bool bucketcheck = true;

          // Search histogram with tolerance + if it should be bucketed in with 0 (no modification)
          if (it != sorted_keys.end() 
            && std::abs(deltamass - *it) < 0.0005 // tolerance of 0.0005 Da
            && (deltamass > 0.05 || deltamass < -0.05)) // exclude delta mass of approx. 0
          {
              hist[*it] += 1.0;
              bucketcheck = false;

              auto& charges = charge_states[*it];
              if (std::find(charges.begin(), charges.end(), charge) == charges.end())
              {
                  num_charges_at_mass[*it] += 1.0;
                  charges.push_back(charge);
              }
          }

          // Add new mass shift to histogram, with check if shift should be bucketed towards 0 
          if (bucketcheck && (deltamass > 0.05 || deltamass < -0.05))
          {
              // Insert new key in the sorted vector at the correct position
              sorted_keys.insert(it, deltamass);

              hist[deltamass] = 1.0;
              num_charges_at_mass[deltamass] = 1.0;
              charge_states[deltamass].push_back(charge);
          }
      }

    }
  }

  //Write out the results to a new data structure 
  pair< vector<double>, pair<CountToDeltaMass, CountToDeltaMass>> results; 
  results.second.first = hist; 
  results.second.second = num_charges_at_mass; 
  results.first = sorted_keys; 
  bool with_smoothing = smoothing; 

  if (with_smoothing)
  {
    CountToDeltaMass smoothed_hist =  smoothDeltaMassHist(hist, 0.001);    // KDE on the histogram   
    CountToDeltaMass smoothedMaxes3 = findPeaksInDeltaMassHistogram( smoothed_hist, 0.0, 3.0 ); // Pick local maxima as candidates 
    CountToDeltaMass num_charges_at_mass_smoothed; 

    for (auto& x : smoothedMaxes3)
    {
      num_charges_at_mass_smoothed[x.first] = num_charges_at_mass[x.first]; // Add charges to new smoothed candidates 
    }

    pair<vector <double>, pair<CountToDeltaMass, CountToDeltaMass>> results_smoothed; 
    results_smoothed.second.first = smoothedMaxes3; 
    results_smoothed.second.second = num_charges_at_mass_smoothed; 
    results_smoothed.first = sorted_keys; 

    return results_smoothed; //Return the smoothed results 
  }
  
  return results ; //Return the un-smoothed results 
}

//Fucntion that maps a selection of masses to certain PTMs and returns a summary of said PTMs. Also adds PTM for each petide without in-peptide localization. 
 vector<PeptideIdentification> mapDifftoMods( CountToDeltaMass hist, CountToDeltaMass charge_hist, vector<PeptideIdentification>& pips, double precursor_mass_tolerance_ = 5, bool precursor_mass_tolerance_unit_ppm = true, String outfile = "", vector<double> keys_sorted = {})
{
    
  vector<vector<PeptideIdentification>> clusters(hist.size(), vector<PeptideIdentification>());
  


  //Variables for storing information from the modifications DB 
  map<double, String, FuzzyDoubleComparator> mass_of_mods(FuzzyDoubleComparator(1e-9)); 
  vector<pair<double, String>> mass_of_mods_vec; 

  //Searches modifications 
  vector<String> searchmodifications_names;
  ModificationsDB* mod_db = ModificationsDB::getInstance();
  mod_db->getAllSearchModifications(searchmodifications_names); 
  for (const String& m : searchmodifications_names)
  {
    const ResidueModification* residue = mod_db->getModification(m); 
    String unimod_res = residue->getUniModAccession(); 
    double res_diffmonoMass = residue->getDiffMonoMass(); 
    String res_name = residue->getFullName(); 
    if((res_name.find("substitution") == string::npos) ) mass_of_mods[res_diffmonoMass] = res_name;  
  }  

    //Make a new map with combinations of mods 
    map<double, String, FuzzyDoubleComparator> combo_mods(FuzzyDoubleComparator(1e-9)); 

    for (map<double, String>::const_iterator mit = mass_of_mods.begin(); mit != mass_of_mods.end(); ++mit)
    {
      for (map<double, String>::const_iterator mit2 = mit ; mit2 != mass_of_mods.end(); ++mit2)
      {
        combo_mods[mit->first + mit2->first] = mit->second + "++" + mit2->second; 
      }
    }  
    //Variables for going through the histogram 
    map<double, String>::const_iterator low_it;
    map<double, String>::const_iterator up_it;
    std::vector< map<double, String>> between; 
    StringList modnames; 
    map<String, modification> modifications; 

    map<double, String> hist_found; //Add map between found PTMs  

  //Mapping with tolerances 
  for (double& key : keys_sorted)
  {
    if (hist.find(key) == hist.end()) continue;
    double rate = hist[key];
    double current_cluster_mass = key;
    double lowerbound, upperbound;
    const double epsilon = 1e-8;  // Define a small epsilon for float comparisons

    if (precursor_mass_tolerance_unit_ppm) // ppm
    {
        lowerbound = current_cluster_mass - current_cluster_mass * precursor_mass_tolerance_ * 1e-6;
        upperbound = current_cluster_mass + current_cluster_mass * precursor_mass_tolerance_ * 1e-6;
        precursor_mass_tolerance_ = current_cluster_mass * precursor_mass_tolerance_ * 1e-6;
    }
    else // Dalton
    {
        lowerbound = current_cluster_mass - precursor_mass_tolerance_;
        upperbound = current_cluster_mass + precursor_mass_tolerance_;
    }
    

    //Upper and lower bound search on the modification database 
    auto low_it = mass_of_mods.lower_bound(current_cluster_mass);
    auto up_it = mass_of_mods.upper_bound(current_cluster_mass);

    auto mapped_val = *low_it; 
    auto mapped_val_high = *up_it;   

    if (mapped_val.first == mapped_val_high.first) //Only one mapping found 
    {
      if (((mapped_val.first >= lowerbound - epsilon)  && (mapped_val.first <= upperbound + epsilon))) //Mapping is within bounds
      { 
        modnames.push_back(mapped_val.second); 
        if (modifications.find(mapped_val.second) == modifications.end()) //Modification hasn't already been found
        {
          modification modi{}; 
          modi.mass.push_back(mapped_val.first); 
          modi.rate = rate; 
          modi.numcharges = charge_hist[key]; 
          modifications[mapped_val.second] = modi; 
          hist_found[mapped_val.first] = mapped_val.second; 
        }
        else{ //If for some reason it wasn't found while looking through hist earlier 
          modifications[mapped_val.second].rate += rate;   
          modifications[mapped_val.second].numcharges = max(charge_hist[key], modifications[mapped_val.second].numcharges);
        }
      }
      else //Mapping is out of bounds for one PTM, check combinations of PTMs, largely same code as above 
      { 
        //Change values to matching combinations of mods 
        low_it = combo_mods.lower_bound(current_cluster_mass);
        up_it = combo_mods.upper_bound(current_cluster_mass);
        auto mapped_val_combo = *low_it; 
        auto mapped_val_high_combo = *up_it; 

        if (mapped_val_combo.first == mapped_val_high_combo.first) //Only one mapping found 
        {
          //Control-flow checks 
          bool combocheck = true; 
          bool breakcheck = false; 
          if ( !(hist_found.empty())) //Check if the modification can be explained through an already discovered (single) modificatiion 
          { 
            for (map<double, OpenMS::String>::const_iterator hit = hist_found.begin(); hit != hist_found.end(); ++hit)
            {
              if (abs(hit->first - current_cluster_mass) < precursor_mass_tolerance_ )
              {
                modifications[hit->second].rate += rate;   
                modifications[hit->second].numcharges = max(charge_hist[key], modifications[hit->second].numcharges);
                breakcheck = true; 
                combocheck = false; 
              }
              if (abs((hit->first + 1) - current_cluster_mass ) < precursor_mass_tolerance_ ) //Check if it can be explained by an isotope of a previous modification (+1Da)
              {
                modification modi{}; 
                modi.mass.push_back(hit->first + 1); 
                modi.rate = rate; 
                modi.numcharges = charge_hist[key]; 
                String mod_temp_name = hit->second + "+1Da"; 
                modifications[mod_temp_name ] = modi; 
                hist_found[hit->first + 1] = hit->second + "+1Da"; 
                breakcheck = true; 
                combocheck = false; 
              }
              if (breakcheck) break; 
            }
          }
          //Perform a more stringent check on the combinations, due to the amount of combinations, more false positive matches possible 
          if (((mapped_val_combo.first >= current_cluster_mass - precursor_mass_tolerance_/10)  && (mapped_val_combo.first <= current_cluster_mass + precursor_mass_tolerance_/10)) && combocheck )
          { 
            modnames.push_back(mapped_val_combo.second); 
            if (modifications.find(mapped_val_combo.second) == modifications.end()) //Modification hasn't already been found
            {
              modification modi{}; 
              modi.mass.push_back(mapped_val_combo.first);
              modi.rate = rate; 
              modi.numcharges = charge_hist[key]; 
              modifications[mapped_val_combo.second] = modi; 
            }
            else
            {
              modifications[mapped_val_combo.second].rate += rate;   
              modifications[mapped_val_combo.second].numcharges = max(charge_hist[key], modifications[mapped_val_combo.second].numcharges);
            }
          }
        }

        else //PTM is not explained by individual PTM or combination of two PTMs. 
        {
          String unknownmod_name = "Unknown" + std::to_string(std::round(current_cluster_mass)); 
          if (modifications.find(unknownmod_name ) == modifications.end()) //Modification hasn't already been found
          {
            modification modi{};  //current_cluster_mass
            modi.mass.push_back(current_cluster_mass); 
            modi.rate = rate; 
            modi.numcharges = charge_hist[key]; 
            modifications[unknownmod_name] = modi; 
          }

          else 
          {
            modifications[unknownmod_name].rate += rate;   
            modifications[unknownmod_name].numcharges = max(charge_hist[key], modifications[unknownmod_name].numcharges);
          }
        }
      }  
    }   
    else //More than one mapping found 
    {
      if ( (mapped_val.first >= lowerbound  && mapped_val_high.first <= upperbound) )
      {
        String mod_mix_name = mapped_val.second + "/" + mapped_val_high.second; 

        if (modifications.find(mod_mix_name) == modifications.end()) //Modification hasn't already been found
        {
          modification modi{}; 
          modi.mass.push_back(mapped_val.first); //current_cluster_mass
          modi.mass.push_back(mapped_val_high.first); 
          modi.rate = rate; 
          modi.numcharges = charge_hist[key]; 
          modifications[mod_mix_name] = modi; 
        }
        else 
        {
          modifications[mod_mix_name].rate += rate;   
          modifications[mod_mix_name].numcharges = max(charge_hist[key], modifications[mod_mix_name].numcharges);
        }
      }
      else //No mapping found for mass shift 
      {
        String unknownmod_name = "Unknown" + std::to_string(std::round(current_cluster_mass)); 
        if (modifications.find(unknownmod_name ) == modifications.end()) //Modification hasn't already been found
        {
          modification modi{}; 
          modi.mass.push_back(current_cluster_mass); //current_cluster_mass
          modi.mass.push_back(current_cluster_mass); 
          modi.rate = rate; 
          modi.numcharges = charge_hist[key]; 
          modifications[unknownmod_name] = modi; 
        }
        else 
        {
          modifications[unknownmod_name].rate += rate;   
          modifications[unknownmod_name].numcharges = max(charge_hist[key], modifications[unknownmod_name].numcharges);
        }
      }
    }
  }

  vector<pair<double, pair<String, pair<double, vector<double>>>>> pairs_by_rate; 

  //adds the various modification data to one structure 
  for (map<String, modification>::const_iterator modit = modifications.begin(); modit != modifications.end(); ++modit)
  {
    //Charge and mass(es) pair
    pair<double, vector<double>> pair0; 
    pair0.first = modit->second.numcharges; 
    pair0.second = modit->second.mass; 
    
    //Name + (charge and mass)
    pair<String, pair<double, vector<double>>> pair1; 
    pair1.first = modit->first; 
    pair1.second = pair0; 

    //Rate + (Name + (Charge + Mass))
    pair<double, pair<String, pair<double, vector<double>>>> pair2; 
    pair2.first = modit->second.rate; 
    pair2.second = pair1; 


    pairs_by_rate.push_back(pair2); 
  }

  sort(pairs_by_rate.begin(), pairs_by_rate.end(), [=](std::pair<double, pair<String, pair<double, vector<double>>>>& a, std::pair<double, pair<String, pair<double, vector<double>>>>& b)
  {
    return a.second.second.first + a.first > b.second.second.first + b.first;
  }
  );

  //Add the modifications to the output for each peptide 
  vector<PeptideIdentification> finalmodifiedpeptides; 

  for (auto& id : pips)
  {
    auto& hits = id.getHits();
    for (auto& h : hits)
    {
      double expval = std::stod(h.getMetaValue("SAGE:ExpMass"));
      double calcval = std::stod(h.getMetaValue("SAGE:CalcMass"));
      double deltamass = expval - calcval;
      String PTM = ""; 
      bool bucketcheck = true; 

      for (map<double, String>::const_iterator mit = hist_found.begin(); mit != hist_found.end(); ++mit)
          {    
            //Check with error tolerance in buckets if already present in histogram 
            if(deltamass <  mit->first+0.01 && deltamass >  mit->first-0.01 && bucketcheck && ((0.05 <  deltamass) || (-0.05 >  deltamass)))
            {
              PTM = mit->second; 
              bucketcheck = false; 
            }
          }
      if (bucketcheck)
      {
        PTM = "Unknown" + String(deltamass); 
      }
      h.setMetaValue("PTM: ", PTM);
    }
  }
  

    //Remove idxml from output file name and write table 
  String output_tab = outfile.substr(0, outfile.size()-5) + "_OutputTable.tsv"; 
  try
  {
    std::ofstream outfile(output_tab);
    // Check if the file was opened successfully
    if (!outfile.is_open()) {
        std::cerr << "Error opening file: " << output_tab << std::endl;
    }

    outfile << "Name" << '\t' << "Mass" << '\t' << "Modified Peptides (incl. charge variants)" << '\t' << "Modified Peptides" << '\n'; 
    // Iterate over the data and write to the file
    for (const auto& x : pairs_by_rate) 
    { 
      //Check if there is a pair of candidates or just one
      if (x.second.second.second.size() < 2)
      {
        outfile <<  x.second.first << '\t' << x.second.second.second.at(0) << '\t' << std::round(x.second.second.first +  x.first) << '\t' << std::round(x.first)  << '\n'; 
      }
      else
      {
        outfile <<  x.second.first << '\t' << x.second.second.second.at(0) << "/" << x.second.second.second.at(1) << '\t' << std::round(x.second.second.first +  x.first) << '\t' << std::round(x.first)  << '\n'; 
      }
    }
    // Close the file
    outfile.close();
  }
  catch(Exception::FileNotFound& e)
  {
    cout << "File could not be found! " << std::endl; 
  } 



  //Return the peptides with the additional PTM column 
  return finalmodifiedpeptides; 
} 


protected:
  // create a template-based configuration file for sage
  // variable values correspond to sage parameter that can be configured via TOPP tool parameter.
  // values will be pasted into the config_template at the corresponding tag. E.g. bucket_size at tag ##bucket_size##
  static constexpr size_t bucket_size = 8192;
  static constexpr size_t min_len = 5; 
  static constexpr size_t max_len = 50; 
  static constexpr size_t missed_cleavages = 2;
  static constexpr double fragment_min_mz = 200.0;
  static constexpr double fragment_max_mz = 2000.0;
  static constexpr double peptide_min_mass = 500.0;
  static constexpr double peptide_max_mass = 5000.0;
  static constexpr size_t min_ion_index = 2;
  static constexpr size_t max_variable_mods = 2;
  const std::string precursor_tol_unit = "ppm";
  static constexpr double precursor_tol_left = -6.0;
  static constexpr double precursor_tol_right = 6.0;
  const std::string fragment_tol_unit = "ppm";
  static constexpr double fragment_tol_left = -10.0;
  static constexpr double fragment_tol_right = 10.0;
  const std::string isotope_errors = "-1, 3";
  const std::string charges_if_not_annotated = "2, 5";
  static constexpr size_t min_matched_peaks = 6;
  static constexpr size_t report_psms = 1;
  static constexpr size_t min_peaks = 15;
  static constexpr size_t max_peaks = 150;

  std::string config_template = R"(
{
  "database": {
    "bucket_size": ##bucket_size##,
    "enzyme": {
      "missed_cleavages": ##missed_cleavages##,
      "min_len": ##min_len##,
      "max_len": ##max_len##,
      ##enzyme_details##
    },
    "fragment_min_mz": ##fragment_min_mz##,
    "fragment_max_mz": ##fragment_max_mz##,
    "peptide_min_mass": ##peptide_min_mass##,
    "peptide_max_mass": ##peptide_max_mass##,
    "ion_kinds": ["b", "y"],
    "min_ion_index": ##min_ion_index##,
    "static_mods": {
      ##static_mods##
    },
    "variable_mods": {
      ##variable_mods##
    },
    "max_variable_mods": ##max_variable_mods##,
    "generate_decoys": false,
    "decoy_tag": "##decoy_prefix##"
  },
  "precursor_tol": {
    "##precursor_tol_unit##": [
      ##precursor_tol_left##,
      ##precursor_tol_right##
    ]
  },
  "fragment_tol": {
    "##fragment_tol_unit##": [
    ##fragment_tol_left##,
    ##fragment_tol_right##
    ]
  },
  "precursor_charge": [
    ##charges_if_not_annotated##
  ],
  "isotope_errors": [
    ##isotope_errors##
  ],
  "deisotope": ##deisotope##,
  "chimera": ##chimera##,
  "predict_rt": ##predict_rt##,
  "min_peaks": ##min_peaks##,
  "max_peaks": ##max_peaks##,
  "min_matched_peaks": ##min_matched_peaks##,
  "report_psms": ##report_psms##, 
  "wide_window": ##wide_window##
}
)";

  // formats a single mod entry as sage json entry
  String getModDetails(const ResidueModification* mod, const Residue* res)
  {
    String origin;
    if (mod->getTermSpecificity() == ResidueModification::N_TERM)
    { 
      origin += "^";
    }
    else if (mod->getTermSpecificity() == ResidueModification::C_TERM)
    {
      origin += "$";
    }
    else if (mod->getTermSpecificity() == ResidueModification::PROTEIN_N_TERM)
    {
      origin += "[";
    }
    else if (mod->getTermSpecificity() == ResidueModification::PROTEIN_C_TERM)
    {
      origin += "]";
    }
   if (res != nullptr && res->getOneLetterCode() != "X") // omit letter for "any AA"
   {
     origin += res->getOneLetterCode();
   }

    return String("\"") + origin + "\": " + String(mod->getDiffMonoMass());
  }

  // formats all mod entries into a single multi-line json string
  String getModDetailsString(const OpenMS::ModifiedPeptideGenerator::MapToResidueType& mod_map)
  {
    String mod_details;
    for (auto it = mod_map.val.begin(); it != mod_map.val.end(); ++it)
    {
      const auto& mod = it->first;
      const auto& res = it->second;
      mod_details += getModDetails(mod, res);     
      if (std::next(it) != mod_map.val.end())
      {
        mod_details += ",\n";
      }
    }
    return mod_details;
  }

  // impute values into config_template
  // TODO just iterate over all options??
  String imputeConfigIntoTemplate()
  {
    String config_file = config_template;
    config_file.substitute("##bucket_size##", String(getIntOption_("bucket_size")));
    config_file.substitute("##min_len##", String(getIntOption_("min_len")));
    config_file.substitute("##max_len##", String(getIntOption_("max_len")));
    config_file.substitute("##missed_cleavages##", String(getIntOption_("missed_cleavages")));
    config_file.substitute("##fragment_min_mz##", String(getDoubleOption_("fragment_min_mz")));
    config_file.substitute("##fragment_max_mz##", String(getDoubleOption_("fragment_max_mz")));
    config_file.substitute("##peptide_min_mass##", String(getDoubleOption_("peptide_min_mass")));
    config_file.substitute("##peptide_max_mass##", String(getDoubleOption_("peptide_max_mass")));
    config_file.substitute("##min_ion_index##", String(getIntOption_("min_ion_index")));
    config_file.substitute("##max_variable_mods##", String(getIntOption_("max_variable_mods")));
    config_file.substitute("##precursor_tol_unit##", getStringOption_("precursor_tol_unit") == "Da" ? "da" : "ppm"); // sage might expect lower-case "da"
    config_file.substitute("##precursor_tol_left##", String(getDoubleOption_("precursor_tol_left")));
    config_file.substitute("##precursor_tol_right##", String(getDoubleOption_("precursor_tol_right")));
    config_file.substitute("##fragment_tol_unit##", getStringOption_("fragment_tol_unit") == "Da" ? "da" : "ppm"); // sage might expect lower-case "da"
    config_file.substitute("##fragment_tol_left##", String(getDoubleOption_("fragment_tol_left")));
    config_file.substitute("##fragment_tol_right##", String(getDoubleOption_("fragment_tol_right")));
    config_file.substitute("##isotope_errors##", getStringOption_("isotope_error_range"));
    config_file.substitute("##charges_if_not_annotated##", getStringOption_("charges"));
    config_file.substitute("##min_matched_peaks##", String(getIntOption_("min_matched_peaks")));
    config_file.substitute("##min_peaks##", String(getIntOption_("min_peaks")));
    config_file.substitute("##max_peaks##", String(getIntOption_("max_peaks")));
    config_file.substitute("##report_psms##", String(getIntOption_("report_psms")));
    config_file.substitute("##deisotope##", getStringOption_("deisotope")); 
    config_file.substitute("##chimera##", getStringOption_("chimera")); 
    config_file.substitute("##predict_rt##", getStringOption_("predict_rt")); 
    config_file.substitute("##decoy_prefix##", getStringOption_("decoy_prefix")); 
    config_file.substitute("##wide_window##", getStringOption_("wide_window")); 

    
    //Look at decoy handling 

    String enzyme = getStringOption_("enzyme");
    String enzyme_details;
    if (enzyme == "Trypsin")
    {
      enzyme_details = 
   R"("cleave_at": "KR",
      "restrict": "P",
      "c_terminal": true)";
    }
    else if (enzyme == "Trypsin/P")
    {
      enzyme_details = 
   R"("cleave_at": "KR",
      "restrict": null,
      "c_terminal": true)";
    }
    else if (enzyme == "Chymotrypsin")
    {
      enzyme_details = 
   R"("cleave_at": "FWYL",
      "restrict": "P",
      "c_terminal": true)";
    }
    else if (enzyme == "Chymotrypsin/P")
    {
      enzyme_details = 
   R"("cleave_at": "FWYL",
      "restrict": null,
      "c_terminal": true)";
    }
    else if (enzyme == "Arg-C")
    {
      enzyme_details = 
   R"("cleave_at": "R",
      "restrict": "P",
      "c_terminal": true)";
    }
    else if (enzyme == "Arg-C/P")
    {
      enzyme_details = 
   R"("cleave_at": "R",
      "restrict": null,
      "c_terminal": true)";
    }
    else if (enzyme == "Lys-C")
    {
      enzyme_details = 
   R"("cleave_at": "K",
      "restrict": "P",
      "c_terminal": true)";
    }
    else if (enzyme == "Lys-C/P")
    {
      enzyme_details = 
   R"("cleave_at": "K",
      "restrict": null,
      "c_terminal": true)";
    }    
    else if (enzyme == "Lys-N")
    {
      enzyme_details = 
   R"("cleave_at": "K",
      "restrict": null,
      "c_terminal": false)";
    }
    else if (enzyme == "no cleavage")
    {
      enzyme_details = 
   R"("cleave_at": "$")";
    }    
    else if (enzyme == "unspecific cleavage")
    {
      enzyme_details = 
   R"("cleave_at": "")";
    }
    else if (enzyme == "glutamyl endopeptidase")
    {
      enzyme_details =
   R"("cleave_at": "E",
      "restrict": "E",
      "c_terminal":true)";
    }
    else if (enzyme == "leukocyte elastase")
    {
      enzyme_details =
   R"("cleave_at": "ALIV",
      "restrict": null,
      "c_terminal":true)";
    }

    config_file.substitute("##enzyme_details##", enzyme_details);

    
    auto fixed_mods = getStringList_("fixed_modifications");
    set<String> fixed_unique(fixed_mods.begin(), fixed_mods.end());
    fixed_mods.assign(fixed_unique.begin(), fixed_unique.end());   
    ModifiedPeptideGenerator::MapToResidueType fixed_mod_map = ModifiedPeptideGenerator::getModifications(fixed_mods); // std::unordered_map<const ResidueModification*, const Residue*> val;
    String static_mods_details = getModDetailsString(fixed_mod_map);

    auto variable_mods = getStringList_("variable_modifications");
    set<String> variable_unique(variable_mods.begin(), variable_mods.end());
    variable_mods.assign(variable_unique.begin(), variable_unique.end());
    ModifiedPeptideGenerator::MapToResidueType variable_mod_map = ModifiedPeptideGenerator::getModifications(variable_mods);
    String variable_mods_details = getModDetailsString(variable_mod_map);

    //Treat variables as list for sage v0.15 and beyond 

    StringList static_mods_details_list; 
    StringList variable_mods_details_list; 


    String static_mods_details_split = static_mods_details; 
    String variable_mods_details_split = variable_mods_details; 
    static_mods_details_split.split(",", static_mods_details_list); 
    variable_mods_details_split.split(",", variable_mods_details_list); 



  String temp_String_var; 
     for(auto& x : variable_mods_details_list){
      StringList temp_split; 
      x.split(":", temp_split); 
      
      temp_split.insert(temp_split.begin()+1, ":["); 
      temp_split.insert(temp_split.end(), "]"); 
      String temp_split_Str = ""; 

      for(auto& y : temp_split){
        temp_split_Str = temp_split_Str + y; 
      } 
      temp_String_var = temp_String_var + "," + temp_split_Str ; 
    } 

  
    String temp_String_var_Fin = temp_String_var.substr(1, temp_String_var.size()-1); 
    config_file.substitute("##static_mods##", static_mods_details);
    config_file.substitute("##variable_mods##", temp_String_var_Fin);

    return config_file;
  }

  std::tuple<std::string, std::string, std::string> getVersionNumber_(const std::string& multi_line_input)
  {
      std::regex version_regex("Version ([0-9]+)\\.([0-9]+)\\.([0-9]+)");

      std::sregex_iterator it(multi_line_input.begin(), multi_line_input.end(), version_regex);
      std::smatch match = *it;
      std::cout << "Found Sage version string: " << match.str() << std::endl;      
          
      return make_tuple(it->str(1), it->str(2), it->str(3)); // major, minor, patch
  }

  void registerOptionsAndFlags_() override
  {
    registerInputFileList_("in", "<files>", StringList(), "Input files separated by blank");
    setValidFormats_("in", { "mzML" } );

    registerOutputFile_("out", "<file>", "", "Single output file containing all search results.", true, false);
    setValidFormats_("out", { "idXML" } );

    registerInputFile_("database", "<file>", "", "FASTA file", true, false, {"skipexists"});
    setValidFormats_("database", { "FASTA" } );

    registerInputFile_("sage_executable", "<executable>",
      // choose the default value according to the platform where it will be executed
      #ifdef OPENMS_WINDOWSPLATFORM
        "sage.exe",
      #else
        "sage",
      #endif
      "The Sage executable. Provide a full or relative path, or make sure it can be found in your PATH environment.", true, false, {"is_executable"});

    registerStringOption_("decoy_prefix", "<prefix>", "DECOY_", "Prefix on protein accession used to distinguish decoy from target proteins. NOTE: Decoy suffix is currently not supported by sage.", false, false);
    registerIntOption_("batch_size", "<int>", 0, "Number of files to load and search in parallel (default = # of CPUs/2)", false, false);
    
    registerDoubleOption_("precursor_tol_left", "<double>", -6.0, "Start (left side) of the precursor tolerance window w.r.t. precursor location. Usually used with negative values smaller or equal to the 'right' counterpart.", false, false);
    registerDoubleOption_("precursor_tol_right", "<double>", 6.0, "End (right side) of the precursor tolerance window w.r.t. precursor location. Usually used with positive values larger or equal to the 'left' counterpart.", false, false);
    registerStringOption_("precursor_tol_unit", "<unit>", "ppm", "Unit of precursor tolerance (ppm or Da)", false, false);
    setValidStrings_("precursor_tol_unit", ListUtils::create<String>("ppm,Da"));

    registerDoubleOption_("fragment_tol_left", "<double>", -20.0, "Start (left side) of the fragment tolerance window w.r.t. precursor location. Usually used with negative values smaller or equal to the 'right' counterpart.", false, false);
    registerDoubleOption_("fragment_tol_right", "<double>", 20.0, "End (right side) of the fragment tolerance window w.r.t. precursor location. Usually used with positive values larger or equal to the 'left' counterpart.", false, false);
    registerStringOption_("fragment_tol_unit", "<unit>", "ppm", "Unit of fragment tolerance (ppm or Da)", false, false);
    setValidStrings_("fragment_tol_unit", ListUtils::create<String>("ppm,Da"));

    // add advanced options
    registerIntOption_("min_matched_peaks", "<int>", min_matched_peaks, "Minimum number of b+y ions required to match for PSM to be reported", false, true);
    registerIntOption_("min_peaks", "<int>", min_peaks, "Minimum number of peaks required for a spectrum to be considered", false, true);
    registerIntOption_("max_peaks", "<int>", max_peaks, "Take the top N most intense MS2 peaks only for matching", false, true);
    registerIntOption_("report_psms", "<int>", report_psms, "Number of hits (PSMs) to report for each spectrum", false, true);
    registerIntOption_("bucket_size", "<int>", bucket_size, "How many fragments are in each internal mass bucket (default: 8192 for hi-res data). Try increasing it to 32k or 64k for low-res. See also: fragment_tol_*", false, true);
    registerIntOption_("min_len", "<int>", min_len, "Minimum peptide length", false, true);
    registerIntOption_("max_len", "<int>", max_len, "Maximum peptide length", false, true);
    registerIntOption_("missed_cleavages", "<int>", missed_cleavages, "Number of missed cleavages", false, true);
    registerDoubleOption_("fragment_min_mz", "<double>", fragment_min_mz, "Minimum fragment m/z", false, true);
    registerDoubleOption_("fragment_max_mz", "<double>", fragment_max_mz, "Maximum fragment m/z", false, true);
    registerDoubleOption_("peptide_min_mass", "<double>", peptide_min_mass, "Minimum monoisotopic peptide mass to consider a peptide from the DB", false, true);
    registerDoubleOption_("peptide_max_mass", "<double>", peptide_max_mass, "Maximum monoisotopic peptide mass to consider a peptide from the DB", false, true);
    registerIntOption_("min_ion_index", "<int>", min_ion_index, "Minimum ion index to consider for preliminary scoring. Default = 2 to skip b1/y1 AND (sic) b2/y2 ions that are often missing.", false, true);
    registerIntOption_("max_variable_mods", "<int>", max_variable_mods, "Maximum number of variable modifications", false, true);  
    registerStringOption_("isotope_error_range", "<start,end>", isotope_errors, "Range of (C13) isotope errors to consider for precursor."
      "Can be negative. E.g. '-1,3' for considering '-1/0/1/2/3'", false, true);
    registerStringOption_("charges", "<start,end>", charges_if_not_annotated, "Range of precursor charges to consider if not annotated in the file."
      , false, true);
    

    //Search Enzyme
    vector<String> all_enzymes;
    ProteaseDB::getInstance()->getAllNames(all_enzymes);
    registerStringOption_("enzyme", "<cleavage site>", "Trypsin", "The enzyme used for peptide digestion.", false, false);
    setValidStrings_("enzyme", all_enzymes);

    //Modifications
    vector<String> all_mods;
    ModificationsDB::getInstance()->getAllSearchModifications(all_mods);
    registerStringList_("fixed_modifications", "<mods>", ListUtils::create<String>("Carbamidomethyl (C)", ','), "Fixed modifications, specified using Unimod (www.unimod.org) terms, e.g. 'Carbamidomethyl (C)' or 'Oxidation (M)'", false);
    setValidStrings_("fixed_modifications", all_mods);
    registerStringList_("variable_modifications", "<mods>", ListUtils::create<String>("Oxidation (M)", ','), "Variable modifications, specified using Unimod (www.unimod.org) terms, e.g. 'Carbamidomethyl (C)' or 'Oxidation (M)'", false);
    setValidStrings_("variable_modifications", all_mods);

    //FDR and misc 
    registerDoubleOption_("q_value_threshold", "<double>", 0.01, "The FDR threshhold for filtering peptides", false, false); 
    registerStringOption_("annotate_matches", "<bool>", "true", "If the matches should be annotated (default: false),", false, false); 
    registerStringOption_("deisotope", "<bool>", "false", "Sets deisotope option (true or false), default: false", false, false ); 
    registerStringOption_("chimera", "<bool>", "false", "Sets chimera option (true or false), default: false", false, false  ); 
    registerStringOption_("predict_rt",  "<bool>", "false", "Sets predict_rt option (true or false), default: false", false, false ); 
    registerStringOption_("wide_window", "<bool>", "false", "Sets wide_window option (true or false), default: false", false, false);
    registerStringOption_("smoothing", "<bool>", "false", "Should the PTM histogram be smoothed and local maxima be picked. If false, uses raw data, default: false", false, false);  
    registerIntOption_("threads", "<int>", 1, "Amount of threads available to the program", false, false); 

    // register peptide indexing parameter (with defaults for this search engine)
    registerPeptideIndexingParameter_(PeptideIndexing().getParameters());
  }



  ExitCodes main_(int, const char**) override
  {
    //-------------------------------------------------------------
    // parsing parameters
    //-------------------------------------------------------------

    // do this early, to see if Sage is installed
    String sage_executable = getStringOption_("sage_executable");
    std::cout << sage_executable << " sage executable" << std::endl; 
    String proc_stdout, proc_stderr;
    TOPPBase::ExitCodes exit_code = runExternalProcess_(sage_executable.toQString(), QStringList() << "--help", proc_stdout, proc_stderr, "");
    auto major_minor_patch = getVersionNumber_(proc_stdout);
    String sage_version = std::get<0>(major_minor_patch) + "." + std::get<1>(major_minor_patch) + "." + std::get<2>(major_minor_patch);
    
    //-------------------------------------------------------------
    // run sage
    //-------------------------------------------------------------
    StringList input_files = getStringList_("in");
    String output_file = getStringOption_("out");
    String output_folder = File::path(output_file);
    String fasta_file = getStringOption_("database");
    int batch = getIntOption_("batch_size");
    String decoy_prefix = getStringOption_("decoy_prefix");

    // create config
    String config = imputeConfigIntoTemplate();

    // store config in config_file
    OPENMS_LOG_INFO << "Creating temp file name..." << std::endl;
    String config_file = File::getTempDirectory() + "/" + File::getUniqueName() + ".json";
    OPENMS_LOG_INFO << "Creating Sage config file..." << config_file << std::endl;
    ofstream config_stream(config_file.c_str());
    config_stream << config;
    config_stream.close();

    // keep config file if debug mode is set
    if (getIntOption_("debug") > 1)
    {
      String debug_config_file = output_folder + "/" + File::getUniqueName() + ".json";
      ofstream debug_config_stream(debug_config_file.c_str());
      debug_config_stream << config;
      debug_config_stream.close();     
    }

    String annotation_check;    

    QStringList arguments;

  if( (getStringOption_("annotate_matches").compare("true")) == 0)
  {
    arguments << config_file.toQString() 
              << "-f" << fasta_file.toQString() 
              << "-o" << output_folder.toQString() 
              << "--annotate-matches"
              << "--write-pin"; 
  }
  else
  {
    arguments << config_file.toQString() 
              << "-f" << fasta_file.toQString() 
              << "-o" << output_folder.toQString() 
              << "--write-pin"; 
  }

    if (batch >= 1) arguments << "--batch-size" << QString(batch);
    for (auto s : input_files) arguments << s.toQString();

    OPENMS_LOG_INFO << "Sage command line: " << sage_executable << " " << arguments.join(' ').toStdString() << std::endl;
    
    //std::chrono lines for testing/writing purposes only! 

    #ifdef CHRONOSET
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    #endif      
    // Sage execution with the executable and the arguments StringList
    exit_code = runExternalProcess_(sage_executable.toQString(), arguments);
    #ifdef CHRONOSET
      std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
      std::cout << "Time difference = " << std::chrono::duration_cast<std::chrono::seconds>(end - begin).count() << "[s]" << std::endl;
    #endif

    if (exit_code != EXECUTION_OK)
    {
      std::cout << "Sage executable not found" << std::endl; 
      return exit_code;
    }

    //-------------------------------------------------------------
    // writing IdXML output
    //-------------------------------------------------------------

    // read the sage output
    OPENMS_LOG_INFO << "Reading sage output..." << std::endl;
    StringList filenames;
    StringList extra_scores = {"ln(-poisson)", "ln(delta_best)", "ln(delta_next)", 
      "ln(matched_intensity_pct)", "longest_b", "longest_y", 
      "longest_y_pct", "matched_peaks", "scored_candidates", "CalcMass", "ExpMass" }; 
    double FDR_threshhold = getDoubleOption_("q_value_threshold"); 

    vector<PeptideIdentification> peptide_identifications = PercolatorInfile::load(
      output_folder + "/results.sage.pin",
      true,
      "ln(hyperscore)",
      extra_scores,
      filenames,
      decoy_prefix, 
      FDR_threshhold);

    for (auto& id : peptide_identifications)
    {
      auto& hits = id.getHits();
      for (auto& h : hits)
      {
        for (const auto& meta : extra_scores)
        {
          if (h.metaValueExists(meta))
          {
            h.setMetaValue("SAGE:" + meta, h.getMetaValue(meta));
            h.removeMetaValue(meta);    
          }
        }
      }
    }

    String smoothing_string = getStringOption_("smoothing"); 
    bool smoothing = !(smoothing_string.compare("true")); 

    const pair<vector<double>, pair<CountToDeltaMass, CountToDeltaMass>> resultsClus =  getDeltaClusterCenter(peptide_identifications, smoothing, false); 

    vector<PeptideIdentification> mapD = mapDifftoMods(resultsClus.second.first, resultsClus.second.second, peptide_identifications, 0.01, false, output_file, resultsClus.first); //peptide_identifications; 

    // remove hits without charge state assigned or charge outside of default range (fix for downstream bugs). TODO: remove if all charges annotated in sage
    IDFilter::filterPeptidesByCharge(peptide_identifications, 2, numeric_limits<int>::max());
    
    if (filenames.empty()) filenames = getStringList_("in");

    // TODO: split / merge results and create idXMLs
    vector<ProteinIdentification> protein_identifications(1, ProteinIdentification());

    writeDebug_("write idXMLFile", 1);    
    
    protein_identifications[0].setPrimaryMSRunPath(filenames);  
    protein_identifications[0].setDateTime(DateTime::now());
    protein_identifications[0].setSearchEngine("Sage");
    protein_identifications[0].setSearchEngineVersion(sage_version);

    DateTime now = DateTime::now();
    String identifier("Sage_" + now.get());
    protein_identifications[0].setIdentifier(identifier);
    for (auto & pid : peptide_identifications) 
    { 
      pid.setIdentifier(identifier);
      pid.setScoreType("hyperscore");
      pid.setHigherScoreBetter(true);
    }

    auto& search_parameters = protein_identifications[0].getSearchParameters();
    // protein_identifications[0].getSearchParameters().enzyme_term_specificity = static_cast<EnzymaticDigestion::Specificity>(num_enzyme_termini[getStringOption_("num_enzyme_termini")]);
    protein_identifications[0].getSearchParameters().db = getStringOption_("database");
    
    // add extra scores for percolator rescoring
    vector<String> percolator_features = { "score" };
    for (auto s : extra_scores) percolator_features.push_back("SAGE:" + s);
    search_parameters.setMetaValue("extra_features",  ListUtils::concatenate(percolator_features, ","));
    auto enzyme = *ProteaseDB::getInstance()->getEnzyme(getStringOption_("enzyme"));
    search_parameters.digestion_enzyme = enzyme; // needed for indexing
    search_parameters.enzyme_term_specificity = EnzymaticDigestion::SPEC_FULL;

    search_parameters.charges = "2:5"; // probably hard-coded in sage https://github.com/lazear/sage/blob/master/crates/sage/src/scoring.rs#L301

    search_parameters.mass_type = ProteinIdentification::MONOISOTOPIC;
    search_parameters.fixed_modifications = getStringList_("fixed_modifications");
    search_parameters.variable_modifications = getStringList_("variable_modifications");
    search_parameters.missed_cleavages = getIntOption_("missed_cleavages");
    search_parameters.fragment_mass_tolerance = (getDoubleOption_("fragment_tol_left") + getDoubleOption_("fragment_tol_right")) * 0.5;
    search_parameters.precursor_mass_tolerance = (getDoubleOption_("precursor_tol_left") + getDoubleOption_("precursor_tol_right")) * 0.5;
    search_parameters.precursor_mass_tolerance_ppm = getStringOption_("precursor_tol_unit") == "ppm";
    search_parameters.fragment_mass_tolerance_ppm = getStringOption_("fragment_tol_unit") == "ppm";

    // write all (!) parameters as metavalues to the search parameters
    if (!protein_identifications.empty())
    {
      DefaultParamHandler::writeParametersToMetaValues(this->getParam_(), protein_identifications[0].getSearchParameters(), this->getToolPrefix());
    }

    // if "reindex" parameter is set to true: will perform reindexing
    if (auto ret = reindex_(protein_identifications, peptide_identifications); ret != EXECUTION_OK) return ret;

    map<String,unordered_map<int,String>> file2specnr2nativeid;
    for (const auto& mzml : input_files)
    {
      // TODO stream mzml?
      MzMLFile m;
      MSExperiment exp;
      auto opts = m.getOptions();
      opts.setMSLevels({2,3});
      opts.setFillData(false);
      //opts.setMetadataOnly(true);
      m.setOptions(opts);
      m.load(mzml, exp);
      String nIDType = "";
      if (!exp.getSourceFiles().empty())
      {
        // TODO we could also guess the regex from the first nativeID if it is not stored here
        //  but I refuse to link to Boost::regex just for this
        //  Someone has to rework the API first!
        nIDType = exp.getSourceFiles()[0].getNativeIDTypeAccession();
      }

      for (const auto& spec : exp)
      {
        const String& nID = spec.getNativeID();
        int nr = SpectrumLookup::extractScanNumber(nID, nIDType);
        if (nr >= 0)
        {
          auto [it, inserted] = file2specnr2nativeid.emplace(File::basename(mzml), unordered_map<int,String>({{nr,nID}}));
          if (!inserted)
          {
            it->second.emplace(nr,nID);
          }
        }
      }
    }

    map<Size, String> idxToFile;
    StringList fnInRun;
    protein_identifications[0].getPrimaryMSRunPath(fnInRun);
    Size cnt = 0;
    for (const auto& f : fnInRun)
    {
      idxToFile.emplace(cnt, f);
      ++cnt;
    }

    for (auto& id : peptide_identifications)
    {
      Int64 scanNrAsInt = 0;
      
      try
      { // check if spectrum reference is a string that just contains a number        
        scanNrAsInt = id.getSpectrumReference().toInt64();
        // no exception -> conversion to int was successful. Now lookup full native ID in corresponding file for given spectrum number.
        id.setSpectrumReference( file2specnr2nativeid[idxToFile[id.getMetaValue(Constants::UserParam::ID_MERGE_INDEX)]].at(scanNrAsInt) );                              
      }
      catch (...)
      {
      }
    }
    IdXMLFile().store(output_file, protein_identifications, peptide_identifications);
    return EXECUTION_OK;
  }
};


int main(int argc, const char** argv)
{
  TOPPSageAdapter tool;
  return tool.main(argc, argv);
}

/// @endcond
