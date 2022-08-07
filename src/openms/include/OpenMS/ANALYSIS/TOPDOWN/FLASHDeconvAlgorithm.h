// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2022.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Kyowon Jeong, Jihyung Kim $
// $Authors: Kyowon Jeong, Jihyung Kim $
// --------------------------------------------------------------------------

#pragma once

#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/DATASTRUCTURES/DefaultParamHandler.h>
#include <OpenMS/DATASTRUCTURES//Matrix.h>
#include <OpenMS/ANALYSIS/TOPDOWN/PeakGroup.h>
#include <OpenMS/ANALYSIS/TOPDOWN/FLASHDeconvHelperStructs.h>
#include <OpenMS/ANALYSIS/TOPDOWN/DeconvolvedSpectrum.h>
#include <iostream>
#include <boost/dynamic_bitset.hpp>

namespace OpenMS
{
  /**
  @brief FLASHDeconv algorithm: ultrafast mass deconvolution algorithm for top down mass spectrometry dataset
  From MSSpectrum, this class outputs DeconvolvedSpectrum.
  Deconvolution takes three steps:
   i) decharging and select candidate masses - speed up via binning
   ii) collecting isotopes from the candidate masses and deisotope - peak groups are defined here
   iii) scoring and filter out low scoring masses (i.e., peak groups)
  @ingroup Topdown
*/

  class OPENMS_DLLAPI FLASHDeconvAlgorithm :
      public DefaultParamHandler
  {
  public:
    typedef FLASHDeconvHelperStructs::PrecalculatedAveragine PrecalculatedAveragine;
    typedef FLASHDeconvHelperStructs::LogMzPeak LogMzPeak;

    /// default constructor
    FLASHDeconvAlgorithm();

    /// copy constructor
    FLASHDeconvAlgorithm(const FLASHDeconvAlgorithm& ) = default;

    /// move constructor
    FLASHDeconvAlgorithm(FLASHDeconvAlgorithm&& other) = default;

    /// assignment operator
    FLASHDeconvAlgorithm& operator=(const FLASHDeconvAlgorithm& fd) = default;

    /**
      @brief main deconvolution function that generates the deconvolved spectrum from the original spectrum.
      @param spec the original spectrum
      @param survey_scans the survey scans to assign precursor mass to the deconvolved spectrum.
      @param scan_number scan number is provided from input spectrum to this function in most cases.
      But this parameter is used for real time deconvolution where scan number may be put separately.
      @param precursor_map_for_FLASHIda deconvolved precursor information from FLASHIda
 */
    void performSpectrumDeconvolution(const MSSpectrum& spec,
                                      const std::vector<DeconvolvedSpectrum>& survey_scans,
                                      const int scan_number,
                                      const bool write_detail,
                                      const std::map<int, std::vector<std::vector<double>>>& precursor_map_for_FLASHIda);

    /// return deconvolved spectrum
    DeconvolvedSpectrum& getDeconvolvedSpectrum();

    /// get calculated averagine. This should be called after calculateAveragine is called.
    const PrecalculatedAveragine& getAveragine();

    /// set calculated averagine
    void setAveragine(const PrecalculatedAveragine& avg);

    /// set targeted masses for targeted deconvolution. Masses are targeted in all ms levels
    void setTargetMasses(const std::vector<double>& masses);

    /** @brief precalculate averagine (for predefined mass bins) to speed up averagine generation
        @param use_RNA_averagine if set, averagine for RNA (nucleotides) is calculated
     */
    void calculateAveragine(const bool use_RNA_averagine);

    /// convert double to nominal mass
    static int getNominalMass(const double mass);

    /** calculate cosine between two vectors a and b with additional parameters for fast calculation
     * @param a vector a
     * @param a_start non zero start index of a
     * @param a_end non zero end index of a (exclusive)
     * @param b vector b
     * @param b_size size of b
     * @param offset element index offset between a and b
     */
    static float getCosine(const std::vector<float>& a,
                             int a_start,
                             int a_end,
                             const IsotopeDistribution& b,
                             int b_size,
                             int offset);

    /** @brief Examine intensity distribution over isotope indices. Also determines the most plausible isotope index or, monoisotopic mono_mass
        @param mono_mass monoisotopic mass
        @param per_isotope_intensities per isotope intensity - aggregated through charges
        @param offset output offset between input monoisotopic mono_mass and determined monoisotopic mono_mass
        @param second_best_iso_offset second best scoring isotope offset - for decoy calculation in the future.
        @param avg precalculated averagine
        @param window_width isotope offset value range. If -1, set automatically.
        @param allowed_iso_error allowed isotope error to calculate qscure
        @return calculated cosine similar score
     */
    static float getIsotopeCosineAndDetermineIsotopeIndex(const double mono_mass,
                                                           const std::vector<float>& per_isotope_intensities,
                                                           int& offset,
                                                           int& second_best_iso_offset,
                                                           const PrecalculatedAveragine& avg,
                                                           int window_width = -1, int allowed_iso_error = 1);

  protected:
    void updateMembers_() override;

  private:
    /// FLASHDeconv parameters

    /// minimum isotopologue count in a peak group
    const static int min_iso_size_ = 2;

    /// allowed isotope error in deconvolved mass to calculate qvalue in the future
    int allowed_iso_error_ = 1;

    /// range of RT subject to analysis (in seconds)
    double min_rt_, max_rt_;
    /// range of mz subject to analysis
    double min_mz_, max_mz_;
    /// min charge and max charge subject to analysis, set by users
    int min_abs_charge_, max_abs_charge_;
    /// is positive mode
    bool is_positive_;
    /// to store detailed information
    bool write_detail_ = false;
    /// mass ranges of deconvolution, set by users
    double min_mass_, max_mass_;
    /// current_min_charge_ charge: 1 for MSn n>1; otherwise just min_abs_charge_
    int current_min_charge_;
    /// current_max_charge_: controlled by precursor charge for MSn n>1; otherwise just max_abs_charge_
    int current_max_charge_;
    /// max mass is controlled by precursor mass for MSn n>1; otherwise just max_mass
    double current_max_mass_;
    /// max mass is max_mass for MS1 and 50 for MS2
    double current_min_mass_;
    /// peak intensity threshold subject to analysis
    double intensity_threshold_;
    /// minimum number of peaks supporting a mass
    const IntList min_support_peak_count_ = {3,3,3,3,3,3,3,3};
    /// tolerance in ppm for each MS level
    DoubleList tolerance_;
    /// bin size for first stage of mass selection - for fast convolution, binning is used
    DoubleList bin_width_;
    /// cosine threshold between observed and theoretical isotope patterns for each MS level
    DoubleList min_isotope_cosine_;
    /// max mass count per spectrum for each MS level
    //IntList max_mass_count_;

    /// precalculated averagine distributions for fast averagine generation
    FLASHDeconvHelperStructs::PrecalculatedAveragine avg_;

    /// mass bins that are targeted for FLASHIda global targeting mode
    boost::dynamic_bitset<> target_mass_bins_;
    std::vector<double> target_masses_;

    /// mass bins that are excluded
    boost::dynamic_bitset<> excluded_mass_bins_;
    std::vector<double> excluded_masses_;

    /// harmonic charge factors that will be considered for harmonic mass reduction. For example, 2 is for 1/2 charge harmonic component reduction
    const std::vector<int> harmonic_charges_{2, 3, 5, 7};
    /// Stores log mz peaks
    std::vector<LogMzPeak> log_mz_peaks_;
    /// deconvolved_spectrum_ stores the deconvolved mass peak groups
    DeconvolvedSpectrum deconvolved_spectrum_;
    /// mass_bins_ stores the selected bins for this spectrum + overlapped spectrum (previous a few spectra).
    boost::dynamic_bitset<> mass_bins_;
    /// mz_bins_ stores the binned log mz peaks
    boost::dynamic_bitset<> mz_bins_;
    /// mz_bins_for_edge_effect_ stores the binned log mz peaks, considering edge effect
    boost::dynamic_bitset<> mz_bins_for_edge_effect_;

    /// This stores the "universal pattern"
    std::vector<double> filter_;
    /// This stores the patterns for harmonic reduction
    Matrix<double> harmonic_filter_matrix_;

    /// isotope dalton distance
    double iso_da_distance_;

    /// This stores the "universal pattern" in binned dimension
    std::vector<int> bin_offsets_;
    /// This stores the patterns for harmonic reduction in binned dimension
    Matrix<int> harmonic_bin_offset_matrix_;

    /// minimum mass and mz values representing the first bin of massBin and mzBin, respectively: to save memory space
    double mass_bin_min_value_;
    double mz_bin_min_value_;

    /// current ms Level
    int ms_level_;

    /// high and low charges are differently deconvolved. This value determines the (inclusive) threshold for low charge.
    const int low_charge_ = 6; //10 inclusive

    /// default precursor isolation window size.
    double isolation_window_size_;

    /// allowed maximum peak count per spectrum - intensity based.
    const int max_peak_count_ = 30000;//30000

    /** @brief static function that converts bin to value
        @param bin bin number
        @param min_value minimum value (corresponding to bin number = 0)
        @param bin_width bin width
        @return value corresponding to bin
     */
    static double getBinValue_(const Size bin, const double min_value, const double bin_width);

    /** @brief static function that converts value to bin
        @param value value
        @param min_value minimum value (corresponding to bin number = 0)
        @param bin_width bin width
        @return bin corresponding to value
     */
    static Size getBinNumber_(const double value, const double min_value, const double bin_width);

    ///generate log mz peaks from the input spectrum
    void updateLogMzPeaks_(const MSSpectrum *spec);

    /** @brief generate mz bins and intensity per mz bin from log mz peaks
        @param bin_number number of mz bins
        @param mz_bin_intensities intensity per mz bin
     */
    void updateMzBins_(const Size bin_number, std::vector<float>& mz_bin_intensities);

    ///Generate peak groups from the input spectrum
    void generatePeakGroupsFromSpectrum_();

    /** @brief Update mass_bins_. It select candidate mass bins using the universal pattern, eliminate possible harmonic masses. This function does not perform deisotoping
        @param mz_intensities per mz bin intensity
        @return a matrix containing charge ranges for all found masses
     */
    Matrix<int> updateMassBins_(const std::vector<float>& mz_intensities);

    /** @brief Subfunction of updateMassBins_.
        @param mass_intensities per mass bin intensity
        @return a matrix containing charge ranges for all found masses
     */
    Matrix<int> filterMassBins_(const std::vector<float>& mass_intensities);

    /** @brief Subfunction of updateMassBins_. It select candidate masses and update mass_bins_ using the universal pattern, eliminate possible harmonic masses
        @param mass_intensities mass bin intensities which are updated in this function
        @param mz_intensities mz bin intensities
     */
    void updateCandidateMassBins_(std::vector<float>&  mass_intensities, const std::vector<float>& mz_intensities);

    /** @brief For selected masses in mass_bins_, select the peaks from the original spectrum. Also isotopic peaks are clustered in this function.
        @param per_mass_abs_charge_ranges charge range per mass
     */
    void getCandidatePeakGroups_(const Matrix<int>& per_mass_abs_charge_ranges);

    /// Make the universal pattern.
    void setFilters_();

    /// function for peak group scoring and filtering
    void scoreAndFilterPeakGroups_();

    void removeHarmonicsPeakGroups_(DeconvolvedSpectrum& dpec);

    /// filter out overlapping masses
    void removeOverlappingPeakGroups_(DeconvolvedSpectrum& dpec, const double tol, const int iso_length = 1);

    ///Filter out masses with low isotope cosine scores, only retaining current_max_mass_count masses
    void filterPeakGroupsByIsotopeCosine_(const int current_max_mass_count);

    /**
    @brief register the precursor peak as well as the precursor peak group (or mass) if possible for MSn (n>1) spectrum.
    Given a precursor peak (found in the original MS n-1 Spectrum) the masses containing the precursor peak are searched.
    If multiple masses are detected, the one with the best QScore is selected. For the selected mass, its corresponding peak group (along with precursor peak) is registered.
    If no such mass exists, only the precursor peak is registered.
    @param survey_scans the candidate precursor spectra - the user may allow search of previous N survey scans.
    @param precursor_map_for_real_time_acquisition this contains the deconvolved mass information from FLASHIda runs.
    */
    bool registerPrecursor(const std::vector<DeconvolvedSpectrum>& survey_scans,
                                  const std::map<int, std::vector<std::vector<double>>>& precursor_map_for_real_time_acquisition);

  };
}